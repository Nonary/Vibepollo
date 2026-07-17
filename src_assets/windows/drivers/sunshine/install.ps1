param(
    [switch]$Uninstall,
    [switch]$ValidateOnly,
    [switch]$AllowUnsignedCatalogForValidation,
    [switch]$RegisterVulkanLayerOnly,
    [switch]$UnregisterVulkanLayerOnly,
    [switch]$InstallerBestEffort
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
$script:rebootRequired = $false
$script:virtualDisplayBrokerWasRunning = $false

trap {
    if ($InstallerBestEffort) {
        Write-Warning '[SunshineVirtualDisplay] VIRTUAL_DISPLAY_DRIVER_WARNING: Optional virtual display driver setup did not complete.'
        Write-Warning "[SunshineVirtualDisplay] Installer best-effort driver action failed: $($_.Exception.Message)"
        exit 0
    }

    throw
}

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

function ConvertTo-ProcessArgumentString {
    param([string[]]$ArgumentList = @())

    $quoted = @()
    foreach ($argument in $ArgumentList) {
        $arg = [string]$argument
        if ($arg.Length -eq 0) {
            $quoted += '""'
            continue
        }
        if ($arg -notmatch '[\s"]') {
            $quoted += $arg
            continue
        }

        $builder = [System.Text.StringBuilder]::new()
        [void]$builder.Append('"')
        $backslashes = 0
        foreach ($ch in $arg.ToCharArray()) {
            if ($ch -eq '\') {
                $backslashes++
                continue
            }
            if ($ch -eq '"') {
                [void]$builder.Append(('\' * (($backslashes * 2) + 1)))
                [void]$builder.Append('"')
                $backslashes = 0
                continue
            }
            if ($backslashes -gt 0) {
                [void]$builder.Append(('\' * $backslashes))
                $backslashes = 0
            }
            [void]$builder.Append($ch)
        }
        if ($backslashes -gt 0) {
            [void]$builder.Append(('\' * ($backslashes * 2)))
        }
        [void]$builder.Append('"')
        $quoted += $builder.ToString()
    }

    return ($quoted -join ' ')
}

function Invoke-DriverProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [int[]]$AllowedExitCodes = @(0, 259, 3010),
        [int]$TimeoutSeconds = 300
    )

    $process = Start-Process -FilePath $FilePath -ArgumentList (ConvertTo-ProcessArgumentString -ArgumentList $ArgumentList) -WorkingDirectory $scriptDir -PassThru -WindowStyle Hidden
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        try {
            $process.Kill()
        } catch {
            $null = $_
        }
        throw "[SunshineVirtualDisplay] $FilePath timed out after $TimeoutSeconds seconds."
    }
    if ($process.ExitCode -notin $AllowedExitCodes) {
        throw "[SunshineVirtualDisplay] $FilePath failed with exit code $($process.ExitCode)."
    }
    if ($process.ExitCode -eq 3010) {
        $script:rebootRequired = $true
    }
}

function Invoke-DriverProcessCapture {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [int[]]$AllowedExitCodes = @(0),
        [int]$TimeoutSeconds = 120
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = ConvertTo-ProcessArgumentString -ArgumentList $ArgumentList
    $startInfo.WorkingDirectory = $scriptDir
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        [void]$process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()

        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try {
                $process.Kill()
            } catch {
                $null = $_
            }
            throw "[SunshineVirtualDisplay] $FilePath timed out after $TimeoutSeconds seconds."
        }

        $process.WaitForExit()
        $stdout = if ($stdoutTask.Wait(5000)) { $stdoutTask.Result } else { '' }
        $stderr = if ($stderrTask.Wait(5000)) { $stderrTask.Result } else { '' }
        $output = (@($stdout -split "`r?`n") + @($stderr -split "`r?`n")) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

        if ($process.ExitCode -notin $AllowedExitCodes) {
            $detail = ($output | ForEach-Object { [string]$_ }) -join "`n"
            throw "[SunshineVirtualDisplay] $FilePath failed with exit code $($process.ExitCode). $detail"
        }

        return @($output | ForEach-Object { [string]$_ })
    } finally {
        $process.Dispose()
    }
}

function Invoke-DriverHealthProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [int]$TimeoutSeconds = 120
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = ConvertTo-ProcessArgumentString -ArgumentList $ArgumentList
    $startInfo.WorkingDirectory = $scriptDir
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        [void]$process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()

        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try {
                $process.Kill()
            } catch {
                $null = $_
            }
            return [pscustomobject]@{
                ExitCode = -1
                Output = "$FilePath timed out after $TimeoutSeconds seconds."
            }
        }

        $process.WaitForExit()
        $stdout = if ($stdoutTask.Wait(5000)) { $stdoutTask.Result } else { '' }
        $stderr = if ($stderrTask.Wait(5000)) { $stderrTask.Result } else { '' }
        $output = (@($stdout -split "`r?`n") + @($stderr -split "`r?`n")) |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output = (($output | ForEach-Object { [string]$_ }) -join "`n")
        }
    } finally {
        $process.Dispose()
    }
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

function Assert-CatalogSignature {
    $signature = Get-AuthenticodeSignature -LiteralPath $catPath
    $matchesBundledCertificate = $false
    if ($signature.SignerCertificate -and (Test-Path -LiteralPath $certPath -PathType Leaf)) {
        $bundledCertificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new([System.IO.File]::ReadAllBytes($certPath))
        $matchesBundledCertificate = [string]::Equals($signature.SignerCertificate.Thumbprint, $bundledCertificate.Thumbprint, [System.StringComparison]::OrdinalIgnoreCase)
    }

    if (-not $signature.SignerCertificate -or $signature.Status -eq 'HashMismatch' -or ((-not $matchesBundledCertificate) -and $signature.Status -ne 'Valid')) {
        if ($ValidateOnly -and $AllowUnsignedCatalogForValidation) {
            Write-Warning "[SunshineVirtualDisplay] Driver catalog signature is not valid ($($signature.Status)); validation allowed this local package state: $catPath"
            return
        }
        throw "[SunshineVirtualDisplay] Driver catalog signature is not valid ($($signature.Status)): $catPath"
    }
}

function Assert-Package {
    foreach ($artifact in @($infPath, $dllPath, $catPath, $nefConc, $probePath)) {
        Assert-Artifact -Path $artifact
    }
    if (Test-Path -LiteralPath $certPath -PathType Leaf) {
        Assert-Artifact -Path $certPath
    }

    Assert-VulkanLayerPackage
    Assert-InfContent
    Assert-CatalogSignature
}

function Assert-VulkanLayerPackage {
    foreach ($artifact in @($vulkanLayerDllPath, $vulkanLayerJsonPath)) {
        Assert-Artifact -Path $artifact
    }
}

function Get-VulkanLayerJsonFullPath {
    return (Resolve-Path -LiteralPath $vulkanLayerJsonPath).Path
}

function Open-LocalMachineRegistryKey {
    param(
        [Parameter(Mandatory = $true)]
        [Microsoft.Win32.RegistryView]$View,

        [Parameter(Mandatory = $true)]
        [string]$SubKey,

        [bool]$Writable = $false,
        [bool]$Create = $false
    )

    $baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $View)
    try {
        if ($Create) {
            return $baseKey.CreateSubKey($SubKey, $Writable)
        }

        return $baseKey.OpenSubKey($SubKey, $Writable)
    } finally {
        $baseKey.Dispose()
    }
}

function Register-VulkanLayer {
    Unregister-VulkanLayer
    $jsonFullPath = Get-VulkanLayerJsonFullPath
    $key = Open-LocalMachineRegistryKey `
        -View ([Microsoft.Win32.RegistryView]::Registry64) `
        -SubKey $vulkanImplicitLayersSubKey `
        -Writable $true `
        -Create $true
    if (-not $key) {
        throw "[SunshineVirtualDisplay] Unable to open HKLM:\$vulkanImplicitLayersSubKey in the 64-bit registry view."
    }

    try {
        $key.SetValue($jsonFullPath, 0, [Microsoft.Win32.RegistryValueKind]::DWord)
        Write-Host "[SunshineVirtualDisplay] Vulkan HDR implicit layer registered: $jsonFullPath"
    } finally {
        $key.Dispose()
    }
}

function Unregister-VulkanLayer {
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64, [Microsoft.Win32.RegistryView]::Registry32)) {
        $key = Open-LocalMachineRegistryKey `
            -View $view `
            -SubKey $vulkanImplicitLayersSubKey `
            -Writable $true
        if (-not $key) {
            continue
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
                Write-Host "[SunshineVirtualDisplay] Vulkan HDR implicit layer registrations removed from ${view}: $removed"
            }
        } finally {
            $key.Dispose()
        }
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

function Stop-VirtualDisplayBrokerForDriverInstall {
    $service = Get-Service -Name 'SunshineVirtualDisplayBroker' -ErrorAction SilentlyContinue
    if (-not $service -or $service.Status -eq 'Stopped') {
        return
    }

    $script:virtualDisplayBrokerWasRunning = $true
    Write-Host '[SunshineVirtualDisplay] Stopping virtual display broker before driver replacement.'
    Stop-Service -Name 'SunshineVirtualDisplayBroker' -Force -ErrorAction Stop
    $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
}

function Start-VirtualDisplayBrokerIfNeeded {
    if (-not $script:virtualDisplayBrokerWasRunning) {
        return
    }

    $service = Get-Service -Name 'SunshineVirtualDisplayBroker' -ErrorAction SilentlyContinue
    if (-not $service -or $service.Status -eq 'Running') {
        return
    }

    Write-Host '[SunshineVirtualDisplay] Starting virtual display broker after driver replacement.'
    Start-Service -Name 'SunshineVirtualDisplayBroker' -ErrorAction Stop
    $service.WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
}

function Stop-SunshineVirtualDisplayDriverHost {
    $hosts = @(
        Get-CimInstance Win32_Process -Filter "name='WUDFHost.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -match [regex]::Escape("-DeviceGroupId:$deviceGroupId") }
    )

    foreach ($hostProcess in $hosts) {
        Write-Host "[SunshineVirtualDisplay] Stopping stale UMDF host $($hostProcess.ProcessId) for $deviceGroupId."
        Stop-Process -Id $hostProcess.ProcessId -Force -ErrorAction SilentlyContinue
    }
}

function Restart-SunshineVirtualDisplayRuntime {
    Stop-VirtualDisplayBrokerForDriverInstall
    Stop-SunshineVirtualDisplayDriverHost
    Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices') -AllowedExitCodes @(0, 259, 3010)
    Start-VirtualDisplayBrokerIfNeeded
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

function Test-DeviceNodePresent {
    $enumRoot = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT\DISPLAY'
    if (-not (Test-Path -LiteralPath $enumRoot -PathType Container)) {
        return $false
    }

    foreach ($deviceKey in @(Get-ChildItem -LiteralPath $enumRoot -ErrorAction SilentlyContinue)) {
        $properties = Get-ItemProperty -LiteralPath $deviceKey.PSPath -ErrorAction SilentlyContinue
        if ($properties -and ($properties.HardwareID -contains $hardwareId)) {
            return $true
        }
    }

    return $false
}

function Add-MatchingDriverPublishedName {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$PublishedNames,
        [Parameter(Mandatory = $true)][System.Collections.Generic.HashSet[string]]$ExpectedOriginalNames,
        [string]$PublishedName,
        [string]$OriginalName
    )

    if ([string]::IsNullOrWhiteSpace($PublishedName) -or [string]::IsNullOrWhiteSpace($OriginalName)) {
        return
    }

    $publishedLeaf = [System.IO.Path]::GetFileName($PublishedName.Trim()).ToLowerInvariant()
    $originalLeaf = [System.IO.Path]::GetFileName($OriginalName.Trim())
    if (-not $ExpectedOriginalNames.Contains($originalLeaf)) {
        return
    }

    # Only names issued by Windows for third-party INF packages are safe to
    # pass to pnputil /delete-driver. Never accept a path or arbitrary token
    # from parsed command output.
    if ($publishedLeaf -notmatch '^oem[0-9]+\.inf$') {
        Write-Warning "[SunshineVirtualDisplay] Ignoring unsafe published driver name '$PublishedName'."
        return
    }

    if (-not $PublishedNames.Contains($publishedLeaf)) {
        [void]$PublishedNames.Add($publishedLeaf)
    }
}

function Get-SunshineDriverPublishedNamesFromInfCache {
    $publishedNames = [System.Collections.Generic.List[string]]::new()
    $systemRoot = if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) { 'C:\Windows' } else { $env:SystemRoot }
    $windowsInfRoot = Join-Path $systemRoot 'INF'
    if (-not (Test-Path -LiteralPath $windowsInfRoot -PathType Container)) {
        return @()
    }

    foreach ($publishedInf in @(Get-ChildItem -LiteralPath $windowsInfRoot -File -Filter 'oem*.inf' -ErrorAction SilentlyContinue)) {
        try {
            $content = [System.IO.File]::ReadAllText($publishedInf.FullName)
        } catch {
            continue
        }

        # This fallback is intentionally strict: all three package-owned
        # identifiers must match before an OEM INF can be selected for removal.
        $matchesClass = $content -match '(?im)^\s*ClassGuid\s*=\s*\{4d36e968-e325-11ce-bfc1-08002be10318\}\s*$'
        $matchesCatalog = $content -match '(?im)^\s*CatalogFile(?:\.[^=]+)?\s*=\s*SunshineVirtualDisplayDriver\.cat\s*$'
        $matchesHardware = $content -match '(?im)^\s*[^;\r\n]+\s*=\s*[^,\r\n]+,\s*Root\\SunshineVirtualDisplay\s*$'
        if ($matchesClass -and $matchesCatalog -and $matchesHardware) {
            $publishedName = $publishedInf.Name.ToLowerInvariant()
            if ($publishedName -match '^oem[0-9]+\.inf$' -and -not $publishedNames.Contains($publishedName)) {
                [void]$publishedNames.Add($publishedName)
            }
        }
    }

    return @($publishedNames)
}

function Get-DisplayDriverPublishedNamesByOriginalName {
    param([Parameter(Mandatory = $true)][string[]]$OriginalNames)

    $publishedNames = [System.Collections.Generic.List[string]]::new()
    $expectedOriginalNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $OriginalNames) {
        if (-not [string]::IsNullOrWhiteSpace($name)) {
            [void]$expectedOriginalNames.Add([System.IO.Path]::GetFileName($name.Trim()))
        }
    }

    $machineReadableEnumerationSucceeded = $false
    try {
        $xmlOutput = Invoke-DriverProcessCapture -FilePath $pnputil -ArgumentList @('/enum-drivers', '/class', $classGuid, '/format', 'xml')
        $xmlText = ($xmlOutput | ForEach-Object { [string]$_ }) -join "`n"
        if ([string]::IsNullOrWhiteSpace($xmlText)) {
            throw 'pnputil returned empty XML output.'
        }

        [xml]$document = $xmlText
        if ($null -eq $document.PnpUtil) {
            throw 'pnputil XML output did not contain the expected root element.'
        }

        foreach ($driver in @($document.PnpUtil.Driver)) {
            Add-MatchingDriverPublishedName `
                -PublishedNames $publishedNames `
                -ExpectedOriginalNames $expectedOriginalNames `
                -PublishedName ([string]$driver.DriverName) `
                -OriginalName ([string]$driver.OriginalName)
        }
        $machineReadableEnumerationSucceeded = $true
    } catch {
        Write-Warning "[SunshineVirtualDisplay] Machine-readable driver enumeration failed; using compatibility fallback: $($_.Exception.Message)"
    }

    if (-not $machineReadableEnumerationSucceeded) {
        # Older pnputil versions do not support XML. The capture helper removes
        # blank lines, so a new Published Name must also close the prior record.
        $output = Invoke-DriverProcessCapture -FilePath $pnputil -ArgumentList @('/enum-drivers', '/class', $classGuid)
        $publishedName = ''
        $originalName = ''

        foreach ($line in $output) {
            if ($line -match '^\s*Published Name\s*:\s*(.+?)\s*$') {
                Add-MatchingDriverPublishedName `
                    -PublishedNames $publishedNames `
                    -ExpectedOriginalNames $expectedOriginalNames `
                    -PublishedName $publishedName `
                    -OriginalName $originalName
                $publishedName = $Matches[1]
                $originalName = ''
            } elseif ($line -match '^\s*Original Name\s*:\s*(.+?)\s*$') {
                $originalName = $Matches[1]
            }
        }

        Add-MatchingDriverPublishedName `
            -PublishedNames $publishedNames `
            -ExpectedOriginalNames $expectedOriginalNames `
            -PublishedName $publishedName `
            -OriginalName $originalName

        # The protected Windows INF cache is a language-independent fallback
        # for legacy pnputil builds whose text labels may be localized.
        foreach ($publishedName in @(Get-SunshineDriverPublishedNamesFromInfCache)) {
            if (-not $publishedNames.Contains($publishedName)) {
                [void]$publishedNames.Add($publishedName)
            }
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

function Test-DriverStoreMatchesPackagedPayload {
    $currentDllPaths = @(Get-CurrentDriverStoreDllPaths)
    if ($currentDllPaths.Count -eq 0) {
        return $false
    }

    $packagedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $dllPath).Hash
    $currentHashes = @(
        $currentDllPaths |
            ForEach-Object { (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash } |
            Select-Object -Unique
    )

    return $currentHashes.Count -eq 1 -and
        [string]::Equals($currentHashes[0], $packagedHash, [System.StringComparison]::OrdinalIgnoreCase)
}

function Wait-DriverStoreMatchesPackagedPayload {
    param([int]$TimeoutSeconds = 5)

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    do {
        if (Test-DriverStoreMatchesPackagedPayload) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    } while ($stopwatch.Elapsed -lt [TimeSpan]::FromSeconds($TimeoutSeconds))

    return Test-DriverStoreMatchesPackagedPayload
}

function Write-DriverStorePayloadDiagnostics {
    $packagedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $dllPath).Hash
    Write-Host "[SunshineVirtualDisplay] Packaged driver DLL SHA256: $packagedHash"

    $currentDllPaths = @(Get-CurrentDriverStoreDllPaths)
    if ($currentDllPaths.Count -eq 0) {
        Write-Host '[SunshineVirtualDisplay] DriverStore contains no Sunshine virtual display DLL payloads.'
        return
    }

    foreach ($currentDllPath in $currentDllPaths) {
        try {
            $currentHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $currentDllPath).Hash
            Write-Host "[SunshineVirtualDisplay] DriverStore DLL SHA256: $currentHash ($currentDllPath)"
        } catch {
            Write-Warning "[SunshineVirtualDisplay] Unable to hash DriverStore payload '$currentDllPath': $($_.Exception.Message)"
        }
    }
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

    Write-Host "[SunshineVirtualDisplay] Found Sunshine driver package(s): $($publishedNames -join ', ')."
    foreach ($publishedName in $publishedNames) {
        Write-Host "[SunshineVirtualDisplay] Removing driver package $publishedName."
        Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/delete-driver', $publishedName, '/uninstall', '/force')
    }

    # PnP package bookkeeping can settle just after pnputil exits. Retry any
    # still-registered package once before declaring the clean restage failed.
    Start-Sleep -Milliseconds 500
    $remainingNames = @(Get-SunshineDriverPublishedNames)
    foreach ($publishedName in $remainingNames) {
        Write-Host "[SunshineVirtualDisplay] Retrying removal of driver package $publishedName."
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

function Test-TemporaryVirtualDisplay {
    Write-Host '[SunshineVirtualDisplay] Running temporary display self-test.'
    $result = Invoke-DriverHealthProcess -FilePath $probePath -ArgumentList @('--self-test-temp', '1920', '1080', '60') -TimeoutSeconds 60
    if ($result.ExitCode -eq 0) {
        Write-Host '[SunshineVirtualDisplay] Temporary display self-test passed.'
        return $true
    }

    if (-not [string]::IsNullOrWhiteSpace($result.Output)) {
        Write-Warning "[SunshineVirtualDisplay] Temporary display self-test failed with exit code $($result.ExitCode): $($result.Output)"
    } else {
        Write-Warning "[SunshineVirtualDisplay] Temporary display self-test failed with exit code $($result.ExitCode)."
    }
    return $false
}

function Get-SunshineDeviceInstanceId {
    try {
        $output = Invoke-DriverProcessCapture -FilePath $pnputil -ArgumentList @('/enum-devices', '/deviceid', $hardwareId, '/deviceids', '/drivers') -AllowedExitCodes @(0) -TimeoutSeconds 120
        foreach ($line in $output) {
            if ($line -match '^\s*Instance ID\s*:\s*(.+?)\s*$') {
                return $Matches[1].Trim()
            }
        }
    } catch {
        Write-Warning $_.Exception.Message
    }

    return $null
}

function Reset-SunshineVirtualDisplayDeviceNode {
    Write-Host '[SunshineVirtualDisplay] Recreating stale device node after failed runtime revive.'

    try {
        Stop-VirtualDisplayBrokerForDriverInstall
        Remove-DeviceNode

        try {
            Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices') -AllowedExitCodes @(0, 259, 3010)
        } catch {
            Write-Warning $_.Exception.Message
        }

        Invoke-DriverProcess -FilePath $nefConc -ArgumentList @('--create-device-node', '--class-name', 'Display', '--class-guid', $classGuid, '--hardware-id', $hardwareId)
        Install-DriverPackage
        Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices') -AllowedExitCodes @(0, 259, 3010)
        Initialize-DriverStateRegistryAccess
        Start-VirtualDisplayBrokerIfNeeded

        if (Test-TemporaryVirtualDisplay) {
            $script:rebootRequired = $false
            Write-Host '[SunshineVirtualDisplay] Device-node recreation restored the virtual display driver without a restart.'
            return $true
        }
    } catch {
        Write-Warning "[SunshineVirtualDisplay] Device-node recreation failed: $($_.Exception.Message)"
    } finally {
        try {
            Start-VirtualDisplayBrokerIfNeeded
        } catch {
            Write-Warning $_.Exception.Message
        }
    }

    return $false
}

function Invoke-InstallerHealthCheck {
    if (Test-TemporaryVirtualDisplay) {
        $script:rebootRequired = $false
        return
    }

    $instanceId = Get-SunshineDeviceInstanceId
    if ([string]::IsNullOrWhiteSpace($instanceId)) {
        Write-Warning "[SunshineVirtualDisplay] Unable to resolve device instance ID for $hardwareId; restart is required."
        $script:rebootRequired = $true
    } else {
        Write-Host "[SunshineVirtualDisplay] Restarting device instance after failed temporary display self-test: $instanceId"
        $restart = Invoke-DriverHealthProcess -FilePath $pnputil -ArgumentList @('/restart-device', $instanceId) -TimeoutSeconds 120
        if ($restart.ExitCode -eq 3010) {
            $script:rebootRequired = $true
        } elseif ($restart.ExitCode -ne 0) {
            if (-not [string]::IsNullOrWhiteSpace($restart.Output)) {
                Write-Warning "[SunshineVirtualDisplay] pnputil restart-device failed with exit code $($restart.ExitCode): $($restart.Output)"
            } else {
                Write-Warning "[SunshineVirtualDisplay] pnputil restart-device failed with exit code $($restart.ExitCode)."
            }
        }

        try {
            Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices') -AllowedExitCodes @(0, 259, 3010)
        } catch {
            Write-Warning $_.Exception.Message
        }
        if (Test-TemporaryVirtualDisplay) {
            $script:rebootRequired = $false
            return
        }

        Write-Host "[SunshineVirtualDisplay] Disabling/enabling device instance after failed restart revive: $instanceId"
        $disable = Invoke-DriverHealthProcess -FilePath $pnputil -ArgumentList @('/disable-device', $instanceId, '/force') -TimeoutSeconds 120
        if ($disable.ExitCode -eq 3010) {
            $script:rebootRequired = $true
        } elseif ($disable.ExitCode -ne 0) {
            if (-not [string]::IsNullOrWhiteSpace($disable.Output)) {
                Write-Warning "[SunshineVirtualDisplay] pnputil disable-device failed with exit code $($disable.ExitCode): $($disable.Output)"
            } else {
                Write-Warning "[SunshineVirtualDisplay] pnputil disable-device failed with exit code $($disable.ExitCode)."
            }
        }

        $enable = Invoke-DriverHealthProcess -FilePath $pnputil -ArgumentList @('/enable-device', $instanceId) -TimeoutSeconds 120
        if ($enable.ExitCode -eq 3010) {
            $script:rebootRequired = $true
        } elseif ($enable.ExitCode -ne 0) {
            if (-not [string]::IsNullOrWhiteSpace($enable.Output)) {
                Write-Warning "[SunshineVirtualDisplay] pnputil enable-device failed with exit code $($enable.ExitCode): $($enable.Output)"
            } else {
                Write-Warning "[SunshineVirtualDisplay] pnputil enable-device failed with exit code $($enable.ExitCode)."
            }
        }

        try {
            Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices') -AllowedExitCodes @(0, 259, 3010)
        } catch {
            Write-Warning $_.Exception.Message
        }
        if (Test-TemporaryVirtualDisplay) {
            $script:rebootRequired = $false
            return
        }

        Write-Host '[SunshineVirtualDisplay] Recycling virtual display broker and UMDF host after failed PnP revive.'
        try {
            Restart-SunshineVirtualDisplayRuntime
        } catch {
            Write-Warning $_.Exception.Message
        }
        if (Test-TemporaryVirtualDisplay) {
            $script:rebootRequired = $false
            return
        }
    }

    if (Reset-SunshineVirtualDisplayDeviceNode) {
        return
    }

    $script:rebootRequired = $true
    Write-Warning '[SunshineVirtualDisplay] VIRTUAL_DISPLAY_RESTART_REQUIRED: Virtual display driver installed, but Windows restart is required before virtual display can function.'
    Write-Host '[SunshineVirtualDisplay] A reboot is required to finalize driver installation.'
}

if ($RegisterVulkanLayerOnly -and $UnregisterVulkanLayerOnly) {
    throw '[SunshineVirtualDisplay] RegisterVulkanLayerOnly and UnregisterVulkanLayerOnly cannot be used together.'
}

if ($RegisterVulkanLayerOnly) {
    Assert-VulkanLayerPackage
    if ($ValidateOnly) {
        Write-Host '[SunshineVirtualDisplay] Vulkan HDR layer package validated.'
        exit 0
    }

    Assert-Administrator
    Register-VulkanLayer
    Write-Host '[SunshineVirtualDisplay] Vulkan HDR layer registration complete.'
    exit 0
}

if ($UnregisterVulkanLayerOnly) {
    if ($ValidateOnly) {
        Write-Host '[SunshineVirtualDisplay] Vulkan HDR layer unregister action validated.'
        exit 0
    }

    Assert-Administrator
    Unregister-VulkanLayer
    Write-Host '[SunshineVirtualDisplay] Vulkan HDR layer unregister complete.'
    exit 0
}

Assert-Package

$pnputil = Resolve-SystemToolPath -ToolName 'pnputil.exe'

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

$driverPackageRefreshNeeded = Test-DriverPackageRefreshNeeded
$deviceNodePresent = Test-DeviceNodePresent

if ((-not $driverPackageRefreshNeeded) -and $deviceNodePresent) {
    Initialize-DriverStateRegistryAccess
    Invoke-InstallerHealthCheck
    Write-Host '[SunshineVirtualDisplay] Driver install complete.'
    if ($script:rebootRequired) {
        Write-Host '[SunshineVirtualDisplay] A reboot is required to finalize driver installation.'
    }
    exit 0
}

Stop-SunshineForDriverInstall
Stop-VirtualDisplayBrokerForDriverInstall
Install-DriverPackage

if (-not (Wait-DriverStoreMatchesPackagedPayload)) {
    Write-Warning '[SunshineVirtualDisplay] Windows retained stale or mixed Sunshine DriverStore payloads; removing Sunshine driver packages before restaging.'
    Write-DriverStorePayloadDiagnostics
    Remove-DeviceNode
    Remove-DriverPackage
    Install-DriverPackage
    if (-not (Wait-DriverStoreMatchesPackagedPayload -TimeoutSeconds 10)) {
        Write-DriverStorePayloadDiagnostics
        throw '[SunshineVirtualDisplay] DriverStore payload still does not match the packaged driver after clean restaging.'
    }
    Write-Host '[SunshineVirtualDisplay] Clean DriverStore restaging replaced the stale driver payload.'
}

if (-not (Test-DeviceNodePresent)) {
    Write-Host '[SunshineVirtualDisplay] Creating device node.'
    Invoke-DriverProcess -FilePath $nefConc -ArgumentList @('--create-device-node', '--class-name', 'Display', '--class-guid', $classGuid, '--hardware-id', $hardwareId)
    Install-DriverPackage
}

Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices')
Restart-SunshineVirtualDisplayRuntime
Initialize-DriverStateRegistryAccess
Invoke-InstallerHealthCheck

Write-Host '[SunshineVirtualDisplay] Driver install complete.'
if ($script:rebootRequired) {
    Write-Host '[SunshineVirtualDisplay] A reboot is required to finalize driver installation.'
    exit 0
}
