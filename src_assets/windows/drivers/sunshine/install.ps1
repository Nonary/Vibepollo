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
$legacyDriverPackages = @(
    [pscustomobject]@{
        DisplayName = 'SudoVDA'
        OriginalNames = @('SudoVDA.inf')
        HardwareIds = @('Root\SudoMaker\SudoVDA', 'SudoVDA')
        DeviceGroupIds = @('SudoVDAGroup')
    },
    [pscustomobject]@{
        DisplayName = 'MttVDD'
        OriginalNames = @('MttVDD.inf')
        HardwareIds = @('Root\MttVDD', 'MttVDD')
        DeviceGroupIds = @('MttVDDGroup')
    }
)

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
        'HKR,,Security,,"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)"',
        'UmdfExtensions=IddCx0102',
        'SunshineVirtualDisplayGroup'
    )) {
        if ($infText -notlike "*$required*") {
            throw "[SunshineVirtualDisplay] INF is missing expected content: $required"
        }
    }
}

function Assert-LegacyCleanupPlan {
    $legacyExpectations = @(
        [pscustomobject]@{
            DisplayName = 'SudoVDA'
            OriginalName = 'SudoVDA.inf'
            HardwareId = 'Root\SudoMaker\SudoVDA'
            DeviceGroupId = 'SudoVDAGroup'
        },
        [pscustomobject]@{
            DisplayName = 'MttVDD'
            OriginalName = 'MttVDD.inf'
            HardwareId = 'Root\MttVDD'
            DeviceGroupId = 'MttVDDGroup'
        }
    )

    foreach ($expected in $legacyExpectations) {
        $entry = $legacyDriverPackages | Where-Object { $_.DisplayName -eq $expected.DisplayName } | Select-Object -First 1
        if (-not $entry) {
            throw "[SunshineVirtualDisplay] Legacy cleanup plan is missing $($expected.DisplayName)."
        }
        if ($entry.OriginalNames -notcontains $expected.OriginalName) {
            throw "[SunshineVirtualDisplay] Legacy cleanup plan for $($expected.DisplayName) is missing $($expected.OriginalName)."
        }
        if ($entry.HardwareIds -notcontains $expected.HardwareId) {
            throw "[SunshineVirtualDisplay] Legacy cleanup plan for $($expected.DisplayName) is missing $($expected.HardwareId)."
        }
        if ($entry.DeviceGroupIds -notcontains $expected.DeviceGroupId) {
            throw "[SunshineVirtualDisplay] Legacy cleanup plan for $($expected.DisplayName) is missing $($expected.DeviceGroupId)."
        }
    }
}

function Assert-Package {
    foreach ($artifact in @($infPath, $dllPath, $catPath, $nefConc, $probePath)) {
        Assert-Artifact -Path $artifact
    }
    if (Test-Path -LiteralPath $certPath -PathType Leaf) {
        Assert-Artifact -Path $certPath
    }

    Assert-InfContent
    Assert-LegacyCleanupPlan
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

function Install-DriverPackage {
    Write-Host '[SunshineVirtualDisplay] Installing driver package.'
    Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/add-driver', $infPath, '/install')
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

function Remove-LegacyVirtualDisplayDrivers {
    foreach ($legacyPackage in $legacyDriverPackages) {
        foreach ($legacyHardwareId in $legacyPackage.HardwareIds) {
            Remove-DeviceNodeForHardwareId -HardwareId $legacyHardwareId -Label "$($legacyPackage.DisplayName) legacy virtual display"
        }
    }

    foreach ($legacyPackage in $legacyDriverPackages) {
        $publishedNames = @(Get-DisplayDriverPublishedNamesByOriginalName -OriginalNames $legacyPackage.OriginalNames)
        if ($publishedNames.Count -eq 0) {
            Write-Host "[SunshineVirtualDisplay] No legacy $($legacyPackage.DisplayName) driver package was found in the driver store."
            continue
        }

        foreach ($publishedName in $publishedNames) {
            Write-Host "[SunshineVirtualDisplay] Removing legacy $($legacyPackage.DisplayName) driver package $publishedName."
            Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/delete-driver', $publishedName, '/uninstall', '/force')
        }
    }
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
    Remove-DeviceNode
    Remove-DriverPackage
    Remove-CertificateIfPresent -StoreName 'TrustedPublisher'
    Remove-CertificateIfPresent -StoreName 'Root'
    Write-Host '[SunshineVirtualDisplay] Uninstall complete.'
    exit 0
}

Install-CertificateIfPresent -StoreName 'Root'
Install-CertificateIfPresent -StoreName 'TrustedPublisher'

Remove-LegacyVirtualDisplayDrivers
Install-DriverPackage

Write-Host '[SunshineVirtualDisplay] Recreating device node.'
Remove-DeviceNode
Invoke-DriverProcess -FilePath $nefConc -ArgumentList @('--create-device-node', '--class-name', 'Display', '--class-guid', $classGuid, '--hardware-id', $hardwareId)
Install-DriverPackage
Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices')

Write-Host '[SunshineVirtualDisplay] Driver install complete.'
