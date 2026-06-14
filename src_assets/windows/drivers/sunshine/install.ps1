param(
    [switch]$Uninstall,
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $PSCommandPath
$hardwareId = 'Root\SunshineVirtualDisplay'
$classGuid = '{4D36E968-E325-11CE-BFC1-08002BE10318}'
$deviceGroupId = 'SunshineVirtualDisplayGroup'
$nefConc = Join-Path $scriptDir 'nefconc.exe'
$infPath = Join-Path $scriptDir 'SunshineVirtualDisplayDriver.inf'
$dllPath = Join-Path $scriptDir 'SunshineVirtualDisplayDriver.dll'
$catPath = Join-Path $scriptDir 'SunshineVirtualDisplayDriver.cat'
$certPath = Join-Path $scriptDir 'SunshineVirtualDisplayDriver.cer'
$probePath = Join-Path $scriptDir 'virtualdisplay_probe.exe'
$vulkanLayerDir = Join-Path $scriptDir 'vulkan-layer'
$vulkanLayerDllPath = Join-Path $vulkanLayerDir 'VkLayer_sunshine_hdr.dll'
$vulkanLayerJsonPath = Join-Path $vulkanLayerDir 'VkLayer_sunshine_hdr.json'
$vulkanImplicitLayersSubKey = 'SOFTWARE\Khronos\Vulkan\ImplicitLayers'
$userModeDriversSid = 'S-1-5-84-0-0-0-0-0'

function Assert-Administrator {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '[SunshineVirtualDisplay] Administrator privileges are required to install or remove the virtual display driver.'
    }
}

function Resolve-SystemToolPath {
    param([Parameter(Mandatory = $true)][string]$ToolName)

    $systemRoot = if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) { 'C:\Windows' } else { $env:SystemRoot }
    foreach ($candidate in @(
        (Join-Path $systemRoot "Sysnative\$ToolName"),
        (Join-Path $systemRoot "System32\$ToolName"),
        (Join-Path $systemRoot "SysWOW64\$ToolName")
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    return Join-Path $systemRoot "System32\$ToolName"
}

function Invoke-DriverProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [int[]]$AllowedExitCodes = @(0, 259, 3010)
    )

    $quotedArgs = foreach ($argument in $ArgumentList) {
        $arg = [string]$argument
        if ($arg -notmatch '[\s"]') {
            $arg
        } else {
            '"' + ($arg -replace '\\(?=")', '\' -replace '"', '\"') + '"'
        }
    }

    $process = Start-Process -FilePath $FilePath -ArgumentList ($quotedArgs -join ' ') -WorkingDirectory $scriptDir -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -notin $AllowedExitCodes) {
        throw "[SunshineVirtualDisplay] $FilePath failed with exit code $($process.ExitCode)."
    }
}

function Invoke-DriverProcessCapture {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [int[]]$AllowedExitCodes = @(0)
    )

    $output = & $FilePath @ArgumentList 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -notin $AllowedExitCodes) {
        $detail = ($output | ForEach-Object { [string]$_ }) -join "`n"
        throw "[SunshineVirtualDisplay] $FilePath failed with exit code $exitCode. $detail"
    }

    return @($output | ForEach-Object { [string]$_ })
}

function Assert-Artifact {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "[SunshineVirtualDisplay] Required driver artifact missing: $Path"
    }
    if ((Get-Item -LiteralPath $Path).Length -le 0) {
        throw "[SunshineVirtualDisplay] Required driver artifact is empty: $Path"
    }
}

function Assert-InfContent {
    $infText = Get-Content -LiteralPath $infPath -Raw
    foreach ($required in @(
        'Root\SunshineVirtualDisplay',
        'SunshineVirtualDisplayDriver.dll',
        'CatalogFile=SunshineVirtualDisplayDriver.cat',
        'AddInterface={5f894d6c-3a69-48a2-86ef-e4c671932d63},,ControlInterface',
        '[ControlInterface_AddReg]',
        'HKR,,Security,,"D:P(A;;GA;;;SY)(A;;GA;;;S-1-5-80-2333729190-1599198784-3320592948-2337414441-3098439965)"',
        'HKR,,"ConfigVersion",0x00010001,1',
        'UmdfExtensions=IddCx0102',
        'SunshineVirtualDisplayGroup'
    )) {
        if ($infText -notlike "*$required*") {
            throw "[SunshineVirtualDisplay] INF is missing expected content: $required"
        }
    }
}

function Assert-Package {
    foreach ($artifact in @($infPath, $dllPath, $catPath, $nefConc, $probePath, $vulkanLayerDllPath, $vulkanLayerJsonPath)) {
        Assert-Artifact -Path $artifact
    }
    if (Test-Path -LiteralPath $certPath -PathType Leaf) {
        Assert-Artifact -Path $certPath
    }

    Assert-InfContent
}

function Get-VulkanLayerJsonFullPath {
    return (Resolve-Path -LiteralPath $vulkanLayerJsonPath).Path
}

function Register-VulkanLayer {
    $jsonFullPath = Get-VulkanLayerJsonFullPath
    $key = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($vulkanImplicitLayersSubKey, $true)
    if (-not $key) {
        throw "[SunshineVirtualDisplay] Unable to open HKLM:\$vulkanImplicitLayersSubKey."
    }

    try {
        $key.SetValue($jsonFullPath, 0, [Microsoft.Win32.RegistryValueKind]::DWord)
        Write-Host "[SunshineVirtualDisplay] Vulkan HDR implicit layer registered: $jsonFullPath"
    } finally {
        $key.Dispose()
    }
}

function Unregister-VulkanLayer {
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($vulkanImplicitLayersSubKey, $true)
    if (-not $key) {
        return
    }

    try {
        $removed = 0
        foreach ($valueName in @($key.GetValueNames())) {
            if ([System.IO.Path]::GetFileName($valueName) -eq 'VkLayer_sunshine_hdr.json') {
                $key.DeleteValue($valueName, $false)
                $removed++
            }
        }

        if ($removed -gt 0) {
            Write-Host "[SunshineVirtualDisplay] Vulkan HDR implicit layer registrations removed: $removed"
        }
    } finally {
        $key.Dispose()
    }
}

function Install-CertificateIfPresent {
    param([Parameter(Mandatory = $true)][string]$StoreName)

    if (-not (Test-Path -LiteralPath $certPath -PathType Leaf)) {
        Write-Host "[SunshineVirtualDisplay] No certificate found for LocalMachine\$StoreName; continuing."
        return
    }

    $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new([System.IO.File]::ReadAllBytes($certPath))
    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new($StoreName, 'LocalMachine')
    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $existing = $store.Certificates.Find([System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $cert.Thumbprint, $false)
        if ($existing.Count -eq 0) {
            $store.Add($cert)
            Write-Host "[SunshineVirtualDisplay] Certificate installed into LocalMachine\$StoreName."
        } else {
            Write-Host "[SunshineVirtualDisplay] Certificate already present in LocalMachine\$StoreName."
        }
    } finally {
        $store.Close()
    }
}

function Remove-CertificateIfPresent {
    param([Parameter(Mandatory = $true)][string]$StoreName)

    if (-not (Test-Path -LiteralPath $certPath -PathType Leaf)) {
        return
    }

    $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new([System.IO.File]::ReadAllBytes($certPath))
    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new($StoreName, 'LocalMachine')
    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $existing = $store.Certificates.Find([System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $cert.Thumbprint, $false)
        foreach ($entry in $existing) {
            $store.Remove($entry)
            Write-Host "[SunshineVirtualDisplay] Certificate removed from LocalMachine\$StoreName."
        }
    } finally {
        $store.Close()
    }
}

function Stop-SunshineForDriverInstall {
    $service = Get-Service -Name 'SunshineService' -ErrorAction SilentlyContinue
    if ($service -and $service.Status -ne 'Stopped') {
        Write-Host '[SunshineVirtualDisplay] Stopping Sunshine service before driver replacement.'
        Stop-Service -Name 'SunshineService' -Force -ErrorAction Stop
        $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
    }

    foreach ($process in @(Get-Process -Name 'sunshine' -ErrorAction SilentlyContinue)) {
        Write-Host "[SunshineVirtualDisplay] Stopping Sunshine process $($process.Id) before driver replacement."
        Stop-Process -Id $process.Id -Force -ErrorAction Stop
    }
}

function Install-DriverPackage {
    Write-Host '[SunshineVirtualDisplay] Installing driver package.'
    Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/add-driver', $infPath, '/install')
}

function Grant-RegistryKeyAccess {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Identity,
        [switch]$InheritToChildKeys
    )

    $identityReference = if ($Identity -match '^S-\d-\d+(-\d+)+$') {
        [System.Security.Principal.SecurityIdentifier]::new($Identity)
    } else {
        [System.Security.Principal.NTAccount]::new($Identity)
    }
    $rights = [System.Security.AccessControl.RegistryRights]'ReadKey, WriteKey, CreateSubKey, SetValue'
    $inheritance = if ($InheritToChildKeys) {
        [System.Security.AccessControl.InheritanceFlags]::ContainerInherit
    } else {
        [System.Security.AccessControl.InheritanceFlags]::None
    }
    $rule = [System.Security.AccessControl.RegistryAccessRule]::new(
        $identityReference,
        $rights,
        $inheritance,
        [System.Security.AccessControl.PropagationFlags]::None,
        [System.Security.AccessControl.AccessControlType]::Allow
    )

    if ($Path.StartsWith('HKLM:\', [System.StringComparison]::OrdinalIgnoreCase)) {
        $subPath = $Path.Substring('HKLM:\'.Length)
        $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
            $subPath,
            [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
            [System.Security.AccessControl.RegistryRights]::ChangePermissions
        )
        if (-not $key) {
            throw "[SunshineVirtualDisplay] Registry key not found: $Path"
        }
        try {
            $acl = $key.GetAccessControl()
            $acl.SetAccessRule($rule)
            $key.SetAccessControl($acl)
        } finally {
            $key.Dispose()
        }
        return
    }

    $acl = Get-Acl -LiteralPath $Path
    $acl.SetAccessRule($rule)
    Set-Acl -LiteralPath $Path -AclObject $acl
}

function Initialize-DriverStateRegistryAccess {
    $enumRoot = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT\DISPLAY'
    if (-not (Test-Path -LiteralPath $enumRoot -PathType Container)) {
        return
    }

    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        $applied = $false
        foreach ($deviceKey in @(Get-ChildItem -LiteralPath $enumRoot -ErrorAction SilentlyContinue)) {
            $properties = Get-ItemProperty -LiteralPath $deviceKey.PSPath -ErrorAction SilentlyContinue
            if (-not $properties -or -not ($properties.HardwareID -contains $hardwareId)) {
                continue
            }

            $devicePath = 'HKLM:\' + $deviceKey.Name.Substring('HKEY_LOCAL_MACHINE\'.Length)
            $parametersPath = Join-Path $devicePath 'Device Parameters'
            if (-not (Test-Path -LiteralPath $parametersPath -PathType Container)) {
                New-Item -Path $parametersPath -Force -ErrorAction SilentlyContinue | Out-Null
            }
            if (-not (Test-Path -LiteralPath $parametersPath -PathType Container)) {
                continue
            }

            Grant-RegistryKeyAccess -Path $parametersPath -Identity $userModeDriversSid -InheritToChildKeys
            Write-Host "[SunshineVirtualDisplay] Driver state registry access is ready at $parametersPath."
            $applied = $true
        }

        if ($applied) {
            return
        }

        Start-Sleep -Milliseconds 500
    }

    throw '[SunshineVirtualDisplay] Unable to prepare driver state registry access.'
}

function Get-DisplayDriverPublishedNamesByOriginalName {
    param([Parameter(Mandatory = $true)][string[]]$OriginalNames)

    $publishedNames = [System.Collections.Generic.List[string]]::new()
    $output = Invoke-DriverProcessCapture -FilePath $pnputil -ArgumentList @('/enum-drivers', '/class', 'Display')
    $expectedOriginalNames = @($OriginalNames | ForEach-Object { $_.ToLowerInvariant() })

    $publishedName = ''
    $originalName = ''

    foreach ($line in ($output + '')) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            if ($publishedName -and $expectedOriginalNames.Contains($originalName.ToLowerInvariant())) {
                $publishedNames.Add($publishedName)
            }

            $publishedName = ''
            $originalName = ''
            continue
        }

        if ($line -match '^\s*Published Name\s*:\s*(.+?)\s*$') {
            $publishedName = $Matches[1]
        } elseif ($line -match '^\s*Original Name\s*:\s*(.+?)\s*$') {
            $originalName = $Matches[1]
        }
    }

    return @($publishedNames | Select-Object -Unique)
}

function Get-SunshineDriverPublishedNames {
    Get-DisplayDriverPublishedNamesByOriginalName -OriginalNames @('SunshineVirtualDisplayDriver.inf')
}

function Get-CurrentDriverStoreDllPaths {
    $systemRoot = if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) { 'C:\Windows' } else { $env:SystemRoot }
    $driverStoreRoot = Join-Path $systemRoot 'System32\DriverStore\FileRepository'
    if (-not (Test-Path -LiteralPath $driverStoreRoot -PathType Container)) {
        return @()
    }

    return @(
        Get-ChildItem -LiteralPath $driverStoreRoot -Directory -Filter 'sunshinevirtualdisplaydriver.inf_*' -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName 'SunshineVirtualDisplayDriver.dll' } |
            Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
            Select-Object -Unique
    )
}

function Test-DriverPackageRefreshNeeded {
    $publishedNames = @(Get-SunshineDriverPublishedNames)
    if ($publishedNames.Count -eq 0) {
        Write-Host '[SunshineVirtualDisplay] No installed Sunshine virtual display driver package was found; driver install is required.'
        return $true
    }

    $currentDllPaths = @(Get-CurrentDriverStoreDllPaths)
    if ($currentDllPaths.Count -eq 0) {
        Write-Host '[SunshineVirtualDisplay] No DriverStore Sunshine virtual display DLL was found; driver install is required.'
        return $true
    }

    $packagedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $dllPath).Hash
    $currentHashes = @(
        $currentDllPaths |
            ForEach-Object { (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash } |
            Select-Object -Unique
    )

    if ($currentHashes.Count -eq 1 -and [string]::Equals($currentHashes[0], $packagedHash, [System.StringComparison]::OrdinalIgnoreCase)) {
        Write-Host '[SunshineVirtualDisplay] Installed driver package already matches packaged driver payload; skipping driver replacement.'
        return $false
    }

    Write-Host '[SunshineVirtualDisplay] Packaged driver payload differs from the installed driver package; driver replacement is required.'
    return $true
}

function Remove-DriverPackage {
    $publishedNames = @(Get-SunshineDriverPublishedNames)
    if ($publishedNames.Count -eq 0) {
        Write-Host '[SunshineVirtualDisplay] No Sunshine virtual display driver package was found in the driver store.'
        return
    }

    foreach ($publishedName in $publishedNames) {
        Write-Host "[SunshineVirtualDisplay] Removing driver package $publishedName."
        Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/delete-driver', $publishedName, '/uninstall', '/force')
    }
}

function Remove-DeviceNodeForHardwareId {
    param(
        [Parameter(Mandatory = $true)][string]$HardwareId,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $nefConc -PathType Leaf)) {
        Write-Host '[SunshineVirtualDisplay] nefconc.exe is missing; skipping device-node removal.'
        return
    }

    try {
        Write-Host "[SunshineVirtualDisplay] Removing $Label device node for $HardwareId."
        Invoke-DriverProcess -FilePath $nefConc -ArgumentList @('--remove-device-node', '--hardware-id', $HardwareId, '--class-guid', $classGuid)
    } catch {
        Write-Warning $_.Exception.Message
    }
}

function Remove-DeviceNode {
    Remove-DeviceNodeForHardwareId -HardwareId $hardwareId -Label 'Sunshine virtual display'
}

$pnputil = Resolve-SystemToolPath -ToolName 'pnputil.exe'

Assert-Package

if ($ValidateOnly) {
    Write-Host '[SunshineVirtualDisplay] Driver installer package validated.'
    exit 0
}

Assert-Administrator

if ($Uninstall) {
    Write-Host '[SunshineVirtualDisplay] Removing device node.'
    # Let PnP removal unload the UMDF host. Forcing WUDFHost.exe closed records
    # a critical user-mode driver crash event even when the install succeeds.
    Unregister-VulkanLayer
    Remove-DeviceNode
    Remove-DriverPackage
    Remove-CertificateIfPresent -StoreName 'TrustedPublisher'
    Remove-CertificateIfPresent -StoreName 'Root'
    Write-Host '[SunshineVirtualDisplay] Uninstall complete.'
    exit 0
}

Install-CertificateIfPresent -StoreName 'Root'
Install-CertificateIfPresent -StoreName 'TrustedPublisher'
Register-VulkanLayer

if (-not (Test-DriverPackageRefreshNeeded)) {
    Write-Host '[SunshineVirtualDisplay] Driver install complete.'
    exit 0
}

Stop-SunshineForDriverInstall
Remove-DeviceNode
Remove-DriverPackage
Install-DriverPackage

Write-Host '[SunshineVirtualDisplay] Recreating device node.'
Invoke-DriverProcess -FilePath $nefConc -ArgumentList @('--create-device-node', '--class-name', 'Display', '--class-guid', $classGuid, '--hardware-id', $hardwareId)
Install-DriverPackage
Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices')
Initialize-DriverStateRegistryAccess

Write-Host '[SunshineVirtualDisplay] Driver install complete.'
