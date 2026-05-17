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
$catPath = Join-Path $scriptDir 'sudovda.cat'
$dllPath = Join-Path $scriptDir 'SudoVDA.dll'
$script:rebootRequired = $false
$script:driverProbeTimedOut = $false
$driverProbeTimeoutSeconds = 15
$driverStepTimeoutSeconds = 120

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

        $builder = New-Object System.Text.StringBuilder
        [void]$builder.Append('"')
        $backslashes = 0
        foreach ($ch in $arg.ToCharArray()) {
            if ($ch -eq '\') {
                $backslashes++
                continue
            }
            if ($ch -eq '"') {
                if ($backslashes -gt 0) {
                    [void]$builder.Append(('\' * ($backslashes * 2)))
                    $backslashes = 0
                }
                [void]$builder.Append('\"')
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

function Invoke-Process {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$WorkingDirectory = $scriptDir,
        [int]$TimeoutSeconds = 0
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = ConvertTo-ProcessArgumentString -ArgumentList $ArgumentList
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    $timedOut = $false

    try {
        [void]$process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()

        if ($TimeoutSeconds -gt 0) {
            if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
                $timedOut = $true
                try {
                    $process.Kill()
                    $process.WaitForExit(5000) | Out-Null
                } catch {
                    $null = $_
                }
            } else {
                # Ensure async output readers have finished flushing.
                $process.WaitForExit()
            }
        } else {
            $process.WaitForExit()
        }

        $stdout = ''
        $stderr = ''
        try {
            if ($stdoutTask.Wait(5000)) { $stdout = $stdoutTask.Result }
        } catch {
            $stdout = ''
        }
        try {
            if ($stderrTask.Wait(5000)) { $stderr = $stderrTask.Result }
        } catch {
            $stderr = ''
        }

        $exitCode = 1460
        if (-not $timedOut) {
            try {
                $exitCode = $process.ExitCode
            } catch {
                $exitCode = 1
            }
        }

        return [pscustomobject]@{
            ExitCode = $exitCode
            StdOut   = $stdout
            StdErr   = $stderr
            TimedOut = $timedOut
        }
    }
    finally {
        if ($process) {
            $process.Dispose()
        }
    }
}

function Invoke-DriverStep {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$Description
    )

    $result = Invoke-Process -FilePath $FilePath -ArgumentList $ArgumentList -TimeoutSeconds $driverStepTimeoutSeconds

    if ($result.StdOut) {
        Write-Host $result.StdOut.TrimEnd()
    }
    if ($result.StdErr) {
        Write-Host $result.StdErr.TrimEnd()
    }

    if ($result.TimedOut) {
        throw "[SudoVDA] $Description timed out after $driverStepTimeoutSeconds seconds."
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

function Assert-RequiredDriverArtifact {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$DisplayName
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "[SudoVDA] Required driver artifact missing: $DisplayName ($Path)"
    }

    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($item.Length -le 0) {
        throw "[SudoVDA] Required driver artifact is empty (0 bytes): $DisplayName ($Path)"
    }
}

function Assert-RequiredInstallArtifacts {
    Assert-RequiredDriverArtifact -Path $nefConc -DisplayName 'nefconc.exe'
    Assert-RequiredDriverArtifact -Path $infPath -DisplayName 'SudoVDA.inf'
    Assert-RequiredDriverArtifact -Path $dllPath -DisplayName 'SudoVDA.dll'
    Assert-RequiredDriverArtifact -Path $catPath -DisplayName 'sudovda.cat'
    Assert-RequiredDriverArtifact -Path $certPath -DisplayName 'sudovda.cer'
}

function Install-Certificate {
    param(
        [Parameter(Mandatory = $true)][string]$StoreName,
        [string]$StoreLocation = 'LocalMachine'
    )

    Assert-RequiredDriverArtifact -Path $certPath -DisplayName 'sudovda.cer'

    try {
        $certBytes = [System.IO.File]::ReadAllBytes($certPath)
        $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certBytes)
    }
    catch {
        throw "[SudoVDA] Failed to load certificate from $certPath. $($_.Exception.Message)"
    }

    if ([string]::IsNullOrWhiteSpace($cert.Thumbprint)) {
        throw "[SudoVDA] Certificate at $certPath is invalid (missing thumbprint)."
    }

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

function Get-PresentSudoVdaDevices {
    try {
        $result = Invoke-Process -FilePath $pnputil -ArgumentList @('/enum-devices', '/class', 'Display', '/connected') -TimeoutSeconds $driverProbeTimeoutSeconds
        if ($result.TimedOut) {
            $script:driverProbeTimedOut = $true
            Write-Warning "[SudoVDA] Timed out while enumerating display devices with pnputil after $driverProbeTimeoutSeconds seconds."
            return @()
        }
        if ($result.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($result.StdOut)) {
            return @()
        }

        $entries = @()
        $current = @{}
        foreach ($line in ($result.StdOut -split "`r?`n")) {
            if ($line -match '^\s*Instance ID\s*:\s*(.+)$') {
                $current['InstanceId'] = $matches[1].Trim()
            }
            elseif ($line -match '^\s*Device Description\s*:\s*(.+)$') {
                $current['DeviceDescription'] = $matches[1].Trim()
            }
            elseif ($line -match '^\s*Manufacturer Name\s*:\s*(.+)$') {
                $current['ManufacturerName'] = $matches[1].Trim()
            }
            elseif ($line -match '^\s*Status\s*:\s*(.+)$') {
                $current['Status'] = $matches[1].Trim()
            }
            elseif ($line -match '^\s*Driver Name\s*:\s*(.+)$') {
                $current['DriverName'] = $matches[1].Trim()
            }
            elseif ($line -match '^\s*$') {
                if ($current.ContainsKey('InstanceId')) {
                    $entries += [pscustomobject]$current
                }
                $current = @{}
            }
        }

        if ($current.ContainsKey('InstanceId')) {
            $entries += [pscustomobject]$current
        }

        return $entries | Where-Object {
            $_.ManufacturerName -like "*SudoMaker*" -or
            $_.DeviceDescription -like "*SudoMaker*" -or
            $_.DeviceDescription -like "*SudoVDA*"
        }
    } catch {
        return @()
    }
}

function Get-InstalledDriverInfo {
    try {
        # Avoid Win32_PnPSignedDriver/Get-PnpDevice here. They can block indefinitely
        # under Windows Installer's hidden SYSTEM PowerShell host and stall the MSI.
        # pnputil returns quickly in the same environment and gives us the driver
        # store version needed to decide whether we can skip reinstalling SudoVDA.
        $driverPackages = @(Get-InstalledDriverPackages -Quiet)
        if ($driverPackages.Count -gt 0) {
            $driverPackage = $driverPackages |
                Sort-Object -Property @{ Expression = { Convert-Version -Version $_.DriverVersion }; Descending = $true }, PublishedName |
                Select-Object -First 1

            return [pscustomobject]@{
                DeviceName     = 'SudoMaker Virtual Display Adapter'
                Manufacturer   = $driverPackage.ProviderName
                DriverVersion  = $driverPackage.DriverVersion
                DeviceID       = $driverPackage.PublishedName
                PublishedName  = $driverPackage.PublishedName
            }
        }

        $devices = @(Get-PresentSudoVdaDevices)
        if ($devices.Count -gt 0) {
            $device = $devices | Select-Object -First 1
            return [pscustomobject]@{
                DeviceName     = $device.DeviceDescription
                Manufacturer   = $device.ManufacturerName
                DriverVersion  = $null
                DeviceID       = $device.InstanceId
                PublishedName  = $device.DriverName
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
    # from previous installations that are no longer functional. Use pnputil
    # instead of Get-PnpDevice because PowerShell's PnP cmdlets can hang under MSI.
    try {
        $devices = @(Get-PresentSudoVdaDevices)
        if ($devices.Count -gt 0) {
            return $true
        }
    } catch {
        $null = $_
    }

    return $false
}

function Get-InstalledDriverPackages {
    param([switch]$Quiet)

    $result = Invoke-Process -FilePath $pnputil -ArgumentList @('/enum-drivers') -TimeoutSeconds $driverProbeTimeoutSeconds
    if (-not $Quiet) {
        Write-ProcessOutput -Result $result
    }

    if ($result.TimedOut) {
        $script:driverProbeTimedOut = $true
        Write-Warning "[SudoVDA] Timed out while enumerating driver packages with pnputil after $driverProbeTimeoutSeconds seconds."
        return @()
    }

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
        elseif ($line -match '^\s*Driver Version\s*:\s*(.+)$') {
            $driverVersionText = $matches[1].Trim()
            $current['DriverVersionText'] = $driverVersionText
            if ($driverVersionText -match '^\S+\s+([0-9]+(?:\.[0-9]+)+)\s*$') {
                $current['DriverVersion'] = $matches[1].Trim()
            } else {
                $current['DriverVersion'] = $driverVersionText
            }
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
    $result = Invoke-Process -FilePath $pnputil -ArgumentList @('/delete-driver', $PublishedName, '/uninstall', '/force') -TimeoutSeconds $driverStepTimeoutSeconds
    Write-ProcessOutput -Result $result
    if ($result.TimedOut) {
        throw "[SudoVDA] Timed out while removing driver package $PublishedName after $driverStepTimeoutSeconds seconds."
    }

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
        $removeResult = Invoke-Process -FilePath $nefConc -ArgumentList @('--remove-device-node', '--hardware-id', $hardwareId, '--class-guid', $classGuid) -TimeoutSeconds $driverStepTimeoutSeconds
        Write-ProcessOutput -Result $removeResult
        switch ($removeResult.ExitCode) {
            0     { }
            3010  { $script:rebootRequired = $true }
            1460  { Write-Warning "[SudoVDA] Remove-device-node timed out after $driverStepTimeoutSeconds seconds. Continuing." }
            default { Write-Warning "[SudoVDA] Remove-device-node returned exit code $($removeResult.ExitCode). Continuing." }
        }
    } else {
        Write-Warning '[SudoVDA] nefconc.exe not found; skipping device-node removal.'
    }

    $driverPackages = @(Get-InstalledDriverPackages)
    if ($driverPackages.Count -eq 0) {
        Write-Host '[SudoVDA] No matching installed driver package found; trying direct INF removal.'
        $fallback = Invoke-Process -FilePath $pnputil -ArgumentList @('/delete-driver', 'SudoVDA.inf', '/uninstall', '/force') -TimeoutSeconds $driverStepTimeoutSeconds
        Write-ProcessOutput -Result $fallback
        switch ($fallback.ExitCode) {
            0     { return }
            3010  { $script:rebootRequired = $true; return }
            1460  { throw "[SudoVDA] Direct INF removal timed out after $driverStepTimeoutSeconds seconds." }
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

Assert-RequiredInstallArtifacts

$targetVersion = Get-TargetDriverVersion
$installedInfo = Get-InstalledDriverInfo
$installedVersion = if ($installedInfo) { $installedInfo.DriverVersion } else { $null }
$installedVersionObj = Convert-Version -Version $installedVersion
$targetVersionObj = Convert-Version -Version $targetVersion

Write-Host "[SudoVDA] Target version: $targetVersion"
Write-Host "[SudoVDA] Installed version: $installedVersion"
Write-Host "[SudoVDA] Driver info found: $($null -ne $installedInfo)"

if ($script:driverProbeTimedOut) {
    Write-Host '[SudoVDA] Driver probe timed out; skipping driver changes to avoid stalling or disrupting a working adapter.'
    exit 0
}

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
