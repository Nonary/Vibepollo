$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $PSCommandPath
$hardwarePrefix = 'ROOT\SUDOMAKER\SUDOVDA'
$hardwareId = $hardwarePrefix.ToLowerInvariant()
$classGuid = '{4D36E968-E325-11CE-BFC1-08002BE10318}'
$nefConc = Join-Path $scriptDir 'nefconc.exe'
$infPath = Join-Path $scriptDir 'SudoVDA.inf'
$certPath = Join-Path $scriptDir 'sudovda.cer'
$pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
$script:rebootRequired = $false

Import-Module PnpDevice -ErrorAction SilentlyContinue | Out-Null

function Invoke-Process {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$WorkingDirectory = $scriptDir
    )

    $stdoutPath = [System.IO.Path]::GetTempFileName()
    $stderrPath = [System.IO.Path]::GetTempFileName()

    try {
        $process = Start-Process -FilePath $FilePath `
                                 -ArgumentList $ArgumentList `
                                 -WorkingDirectory $WorkingDirectory `
                                 -WindowStyle Hidden `
                                 -Wait `
                                 -PassThru `
                                 -RedirectStandardOutput $stdoutPath `
                                 -RedirectStandardError $stderrPath

        $stdout = ''
        $stderr = ''

        if (Test-Path -LiteralPath $stdoutPath) {
            $stdout = Get-Content -Path $stdoutPath -Raw -ErrorAction SilentlyContinue
        }
        if (Test-Path -LiteralPath $stderrPath) {
            $stderr = Get-Content -Path $stderrPath -Raw -ErrorAction SilentlyContinue
        }

        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            StdOut   = $stdout
            StdErr   = $stderr
        }
    }
    finally {
        Remove-Item -LiteralPath $stdoutPath, $stderrPath -ErrorAction SilentlyContinue
    }
}

function Invoke-DriverStep {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$Description
    )

    $result = Invoke-Process -FilePath $FilePath -ArgumentList $ArgumentList

    if ($result.StdOut) {
        Write-Host $result.StdOut.TrimEnd()
    }
    if ($result.StdErr) {
        Write-Host $result.StdErr.TrimEnd()
    }

    switch ($result.ExitCode) {
        0     { return }
        3010  { $script:rebootRequired = $true; return }
        default {
            throw "[SudoVDA] $Description failed with exit code $($result.ExitCode)."
        }
    }
}

function Install-Certificate {
    param(
        [Parameter(Mandatory = $true)][string]$StoreName,
        [string]$StoreLocation = 'LocalMachine'
    )

    $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new([System.IO.File]::ReadAllBytes($certPath))
    $location = [System.Enum]::Parse([System.Security.Cryptography.X509Certificates.StoreLocation], $StoreLocation, $true)
    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new($StoreName, $location)

    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $existing = $store.Certificates.Find([System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $cert.Thumbprint, $false)

        if ($existing.Count -gt 0) {
            Write-Host "[SudoVDA] Certificate already present in $StoreLocation\$StoreName."
            return
        }

        $store.Add($cert)
        Write-Host "[SudoVDA] Certificate installed into $StoreLocation\$StoreName."
    }
    catch {
        throw "[SudoVDA] Failed to install certificate into $StoreLocation\$StoreName. $($_.Exception.Message)"
    }
    finally {
        $store.Close()
    }
}

function Get-TargetDriverVersion {
    try {
        $content = Get-Content -Path $infPath -ErrorAction Stop
    } catch {
        throw '[SudoVDA] Unable to read SudoVDA INF for version check.'
    }

    foreach ($line in $content) {
        if ($line -match '^\s*DriverVer\s*=\s*[^,]+,\s*([0-9\.]+)') {
            return $matches[1].Trim()
        }
    }

    return $null
}

function Get-InstalledDriverInfo {
    try {
        # The device InstanceId is assigned by Windows (e.g. ROOT\DISPLAY\0001), not the hardware ID.
        # Detect by manufacturer or device name instead.
        $driver = Get-CimInstance -ClassName Win32_PnPSignedDriver -ErrorAction Stop |
            Where-Object {
                $_.Manufacturer -like "*SudoMaker*" -or
                $_.DeviceName   -like "*SudoMaker*" -or
                $_.DeviceName   -like "*SudoVDA*"
            } |
            Select-Object -First 1

        if ($driver) {
            return $driver
        }

        $devices = Get-PnpDevice -ErrorAction Stop |
            Where-Object { $_.FriendlyName -like "*SudoMaker*" -or $_.FriendlyName -like "*SudoVDA*" }

        if ($devices) {
            $device = $devices | Select-Object -First 1
            $driver = Get-CimInstance -ClassName Win32_PnPSignedDriver -ErrorAction Stop |
                Where-Object { $_.DeviceID -eq $device.InstanceId } |
                Select-Object -First 1

            if ($driver) {
                return $driver
            }
        }

        return $null
    } catch {
        return $null
    }
}

function Convert-Version {
    param([string]$Version)

    if (-not $Version) {
        return $null
    }

    try {
        return [version]$Version
    } catch {
        return $null
    }
}

function Test-DriverPresent {
    try {
        $devices = Get-PnpDevice -ErrorAction Stop |
            Where-Object { $_.FriendlyName -like "*SudoMaker*" -or $_.FriendlyName -like "*SudoVDA*" }
        if ($devices) {
            return $true
        }
    } catch {
        $null = $_
    }

    try {
        $result = Invoke-Process -FilePath $pnputil -ArgumentList @('/enum-devices', '/class', 'Display')
        if ($result.ExitCode -eq 0 -and $result.StdOut -and $result.StdOut -match 'SudoMaker') {
            return $true
        }
    } catch {
        $null = $_
    }

    return $false
}

if (-not (Test-Path -Path $nefConc -PathType Leaf)) {
    throw '[SudoVDA] Unable to locate nefconc.exe.'
}

if (-not (Test-Path -Path $infPath -PathType Leaf)) {
    throw '[SudoVDA] Unable to locate SudoVDA.inf.'
}

$targetVersion = Get-TargetDriverVersion
$installedInfo = Get-InstalledDriverInfo
$installedVersion = if ($installedInfo) { $installedInfo.DriverVersion } else { $null }
$installedVersionObj = Convert-Version -Version $installedVersion
$targetVersionObj = Convert-Version -Version $targetVersion

Write-Host "[SudoVDA] Target version: $targetVersion"
Write-Host "[SudoVDA] Installed version: $installedVersion"
Write-Host "[SudoVDA] Driver info found: $($null -ne $installedInfo)"

if ($installedInfo -and $installedVersionObj -and $targetVersionObj -and $installedVersionObj -ge $targetVersionObj) {
    Write-Host "[SudoVDA] Driver version $installedVersion already installed; skipping."
    exit 0
}

if ($installedInfo -and (-not $installedVersionObj -or -not $targetVersionObj)) {
    Write-Host "[SudoVDA] Driver present but version info incomplete (installed=$installedVersion, target=$targetVersion); skipping to avoid disrupting a working driver."
    exit 0
}

if (-not $installedInfo -and (Test-DriverPresent)) {
    Write-Host '[SudoVDA] Driver detected via PnP but driver info unavailable; skipping to avoid disrupting a working driver.'
    exit 0
}

if ($installedInfo -and $installedVersion -and $targetVersion) {
    Write-Host "[SudoVDA] Upgrading driver from version $installedVersion to $targetVersion."
}

Install-Certificate -StoreName 'Root'
Install-Certificate -StoreName 'TrustedPublisher'

Write-Host '[SudoVDA] Removing any existing SudoVDA driver.'
Invoke-DriverStep -FilePath $nefConc -ArgumentList @('--remove-device-node', '--hardware-id', $hardwareId, '--class-guid', $classGuid) -Description 'Remove SudoVDA device node'

Write-Host '[SudoVDA] Creating virtual display device.'
Invoke-DriverStep -FilePath $nefConc -ArgumentList @('--create-device-node', '--class-name', 'Display', '--class-guid', $classGuid, '--hardware-id', $hardwareId) -Description 'Create SudoVDA device node'

Write-Host '[SudoVDA] Installing SudoVDA driver.'
Invoke-DriverStep -FilePath $nefConc -ArgumentList @('--install-driver', '--inf-path', 'SudoVDA.inf') -Description 'Install SudoVDA driver'

Write-Host '[SudoVDA] Driver install complete.'
if ($script:rebootRequired) {
    Write-Host '[SudoVDA] A reboot is required to finalize driver installation.'
}

$global:LastExitCode = 0
exit 0
