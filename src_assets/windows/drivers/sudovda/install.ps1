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

    $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certPath)
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
        return Get-CimInstance -ClassName Win32_PnPSignedDriver -ErrorAction Stop |
            Where-Object { $_.DeviceID -like "$hardwarePrefix*" } |
            Select-Object -First 1
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
    $searchPrefix = "$hardwarePrefix*"

    try {
        $devices = Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object { $_.InstanceId -like $searchPrefix }
        if ($devices) {
            return $true
        }
    } catch {
        $null = $_
    }

    try {
        $result = Invoke-Process -FilePath $pnputil -ArgumentList @('/enum-devices', "/instanceid $hardwarePrefix")
        if ($result.ExitCode -eq 0 -and $result.StdOut -and $result.StdOut -match [regex]::Escape($hardwarePrefix)) {
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

if ($installedInfo -and $installedVersionObj -and $targetVersionObj -and $installedVersionObj -ge $targetVersionObj) {
    Write-Host "[SudoVDA] Driver version $installedVersion already installed; skipping."
    exit 0
}

if ($installedInfo -and $installedVersion) {
    if ($targetVersion) {
        Write-Host "[SudoVDA] Upgrading driver from version $installedVersion to $targetVersion."
    } else {
        Write-Host "[SudoVDA] Reinstalling driver; unable to determine target version."
    }
} elseif (Test-DriverPresent) {
    Write-Host '[SudoVDA] Driver detected but version information unavailable; reinstalling.'
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
