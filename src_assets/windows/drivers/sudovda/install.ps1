param(
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $PSCommandPath
$hardwarePrefix = 'ROOT\SUDOMAKER\SUDOVDA'
$hardwareId = $hardwarePrefix.ToLowerInvariant()
$classGuid = '{4D36E968-E325-11CE-BFC1-08002BE10318}'
$nefConc = Join-Path $scriptDir 'nefconc.exe'
$infPath = Join-Path $scriptDir 'SudoVDA.inf'
$certPath = Join-Path $scriptDir 'sudovda.cer'
$script:rebootRequired = $false

Import-Module PnpDevice -ErrorAction SilentlyContinue | Out-Null

function Resolve-SystemToolPath {
    param([Parameter(Mandatory = $true)][string]$ToolName)

    $systemRoot = if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) { 'C:\Windows' } else { $env:SystemRoot }
    $candidates = @(
        (Join-Path -Path $systemRoot -ChildPath ("Sysnative\" + $ToolName))
        (Join-Path -Path $systemRoot -ChildPath ("System32\" + $ToolName))
        (Join-Path -Path $systemRoot -ChildPath ("SysWOW64\" + $ToolName))
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -Path $candidate -PathType Leaf) {
            return $candidate
        }
    }

    return $candidates[1]
}

$pnputil = Resolve-SystemToolPath -ToolName 'pnputil.exe'

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

function Write-ProcessOutput {
    param([Parameter(Mandatory = $true)]$Result)

    if ($Result.StdOut) {
        Write-Host $Result.StdOut.TrimEnd()
    }
    if ($Result.StdErr) {
        Write-Host $Result.StdErr.TrimEnd()
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

        # Only consider devices that are currently present to avoid ghost entries.
        $devices = Get-PnpDevice -PresentOnly -ErrorAction Stop |
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
    # Only check present devices to avoid detecting ghost/phantom entries
    # from previous installations that are no longer functional.
    try {
        $devices = Get-PnpDevice -PresentOnly -ErrorAction Stop |
            Where-Object { $_.FriendlyName -like "*SudoMaker*" -or $_.FriendlyName -like "*SudoVDA*" }
        if ($devices) {
            return $true
        }
    } catch {
        $null = $_
    }

    try {
        $result = Invoke-Process -FilePath $pnputil -ArgumentList @('/enum-devices', '/class', 'Display', '/connected')
        if ($result.ExitCode -eq 0 -and $result.StdOut -and $result.StdOut -match 'SudoMaker') {
            return $true
        }
    } catch {
        $null = $_
    }

    return $false
}

function Get-InstalledDriverPackages {
    $result = Invoke-Process -FilePath $pnputil -ArgumentList @('/enum-drivers')
    Write-ProcessOutput -Result $result

    if ($result.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($result.StdOut)) {
        return @()
    }

    $entries = @()
    $current = @{}
    foreach ($line in ($result.StdOut -split "`r?`n")) {
        if ($line -match '^\s*Published Name\s*:\s*(\S+)') {
            $current['PublishedName'] = $matches[1]
        }
        elseif ($line -match '^\s*Original Name\s*:\s*(.+)$') {
            $current['OriginalName'] = $matches[1].Trim()
        }
        elseif ($line -match '^\s*Provider Name\s*:\s*(.+)$') {
            $current['ProviderName'] = $matches[1].Trim()
        }
        elseif ($line -match '^\s*$') {
            if ($current.ContainsKey('PublishedName')) {
                $entries += [pscustomobject]$current
            }
            $current = @{}
        }
    }

    if ($current.ContainsKey('PublishedName')) {
        $entries += [pscustomobject]$current
    }

    return $entries | Where-Object {
        $_.OriginalName -match '^SudoVDA\.inf$' -or $_.ProviderName -match 'SudoMaker'
    }
}

function Remove-DriverPackage {
    param([Parameter(Mandatory = $true)][string]$PublishedName)

    Write-Host "[SudoVDA] Removing driver package $PublishedName."
    $result = Invoke-Process -FilePath $pnputil -ArgumentList @('/delete-driver', $PublishedName, '/uninstall', '/force')
    Write-ProcessOutput -Result $result

    switch ($result.ExitCode) {
        0     { return }
        3010  { $script:rebootRequired = $true; return }
        default { throw "[SudoVDA] Failed to remove driver package $PublishedName (exit code $($result.ExitCode))." }
    }
}

function Invoke-SudoVdaUninstall {
    if (-not (Test-Path -Path $pnputil -PathType Leaf)) {
        throw '[SudoVDA] Unable to locate pnputil.exe.'
    }

    if (Test-Path -Path $nefConc -PathType Leaf) {
        Write-Host '[SudoVDA] Removing existing SudoVDA device node.'
        $removeResult = Invoke-Process -FilePath $nefConc -ArgumentList @('--remove-device-node', '--hardware-id', $hardwareId, '--class-guid', $classGuid)
        Write-ProcessOutput -Result $removeResult
        switch ($removeResult.ExitCode) {
            0     { }
            3010  { $script:rebootRequired = $true }
            default { Write-Warning "[SudoVDA] Remove-device-node returned exit code $($removeResult.ExitCode). Continuing." }
        }
    } else {
        Write-Warning '[SudoVDA] nefconc.exe not found; skipping device-node removal.'
    }

    $driverPackages = @(Get-InstalledDriverPackages)
    if ($driverPackages.Count -eq 0) {
        Write-Host '[SudoVDA] No matching installed driver package found; trying direct INF removal.'
        $fallback = Invoke-Process -FilePath $pnputil -ArgumentList @('/delete-driver', 'SudoVDA.inf', '/uninstall', '/force')
        Write-ProcessOutput -Result $fallback
        switch ($fallback.ExitCode) {
            0     { return }
            3010  { $script:rebootRequired = $true; return }
            default {
                if (Test-DriverPresent) {
                    throw "[SudoVDA] Direct INF removal failed (exit code $($fallback.ExitCode)) and the driver is still present."
                }
                Write-Host '[SudoVDA] Driver package was already absent.'
                return
            }
        }
    }

    foreach ($driverPackage in $driverPackages) {
        Remove-DriverPackage -PublishedName $driverPackage.PublishedName
    }
}

if ($Uninstall) {
    Invoke-SudoVdaUninstall
    Write-Host '[SudoVDA] Driver uninstall complete.'
    if ($script:rebootRequired) {
        Write-Host '[SudoVDA] A reboot is required to finalize driver removal.'
    }
    $global:LastExitCode = 0
    exit 0
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
    if (Test-DriverPresent) {
        Write-Host "[SudoVDA] Driver present but version info incomplete (installed=$installedVersion, target=$targetVersion); skipping to avoid disrupting a working driver."
        exit 0
    }
    Write-Host "[SudoVDA] Driver info found but device not present (installed=$installedVersion, target=$targetVersion); reinstalling."
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
