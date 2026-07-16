param(
    [switch]$StopSunshineFirst,
    [switch]$SkipProbe,
    [string]$LogPath
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $PSCommandPath
$repoRoot = Split-Path -Parent $scriptDir
$hardwareId = 'Root\SunshineVirtualDisplay'
$controlInterfaceClassGuid = '{5f894d6c-3a69-48a2-86ef-e4c671932d63}'
$probePath = Join-Path $repoRoot 'src_assets\windows\drivers\sunshine\virtualdisplay_probe.exe'

function Test-Administrator {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
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

function Resolve-SystemToolPath {
    param([Parameter(Mandatory = $true)][string]$ToolName)

    $systemRoot = if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) { 'C:\Windows' } else { $env:SystemRoot }
    foreach ($candidate in @(
        (Join-Path $systemRoot "Sysnative\$ToolName"),
        (Join-Path $systemRoot "System32\$ToolName")
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    throw "[SunshineVirtualDisplayRevive] Unable to locate $ToolName."
}

function New-DefaultLogPath {
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    return Join-Path ([System.IO.Path]::GetTempPath()) "vibepollo_vdisplay_revive_$stamp.log"
}

function Write-Log {
    param([AllowEmptyString()][string]$Message)

    if ($null -eq $Message) {
        $Message = ''
    }
    $line = '{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'), $Message
    Write-Host $line
    Add-Content -LiteralPath $script:LogPath -Value $line
}

function Write-OutputBlock {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [string]$Text
    )

    Write-Log "----- $Label -----"
    if ([string]::IsNullOrWhiteSpace($Text)) {
        Write-Log '<no output>'
        return
    }

    foreach ($line in ($Text -split "`r?`n")) {
        Write-Log $line
    }
}

function Invoke-CapturedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [int]$TimeoutSeconds = 120
    )

    Write-Log ("Running: {0} {1}" -f $FilePath, (ConvertTo-ProcessArgumentString -ArgumentList $ArgumentList))

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = ConvertTo-ProcessArgumentString -ArgumentList $ArgumentList
    $startInfo.WorkingDirectory = $repoRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        [void]$process.Start()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try {
                $process.Kill()
            } catch {
                $null = $_
            }
            throw "$FilePath timed out after $TimeoutSeconds seconds."
        }

        $stdout = $process.StandardOutput.ReadToEnd()
        $stderr = $process.StandardError.ReadToEnd()
        $combined = (($stdout, $stderr) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) -join "`r`n"
        Write-OutputBlock -Label "exit=$($process.ExitCode)" -Text $combined

        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = $stdout
            Stderr = $stderr
            Output = $combined
        }
    } finally {
        $process.Dispose()
    }
}

function Invoke-PnpUtil {
    param([Parameter(Mandatory = $true)][string[]]$ArgumentList)

    return Invoke-CapturedProcess -FilePath $script:PnpUtilPath -ArgumentList $ArgumentList -TimeoutSeconds 120
}

function Invoke-ProbeCheck {
    if ($SkipProbe) {
        Write-Log 'Skipping virtualdisplay_probe.exe --self-test-temp because -SkipProbe was specified.'
        return $null
    }
    if (-not (Test-Path -LiteralPath $probePath -PathType Leaf)) {
        Write-Log "virtualdisplay_probe.exe is missing; expected path: $probePath"
        return [pscustomobject]@{
            ExitCode = 2
            Stdout = ''
            Stderr = "Missing probe: $probePath"
            Output = "Missing probe: $probePath"
        }
    }

    return Invoke-CapturedProcess -FilePath $probePath -ArgumentList @('--self-test-temp', '1920', '1080', '60') -TimeoutSeconds 60
}

function Stop-SunshineForDriverRevive {
    $service = Get-Service -Name 'SunshineService' -ErrorAction SilentlyContinue
    if ($service -and $service.Status -ne 'Stopped') {
        Write-Log 'Stopping Sunshine service before driver revive.'
        Stop-Service -Name 'SunshineService' -Force -ErrorAction Stop
        $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
    }

    foreach ($process in @(Get-Process -Name 'sunshine' -ErrorAction SilentlyContinue)) {
        Write-Log "Stopping Sunshine process $($process.Id) before driver revive."
        Stop-Process -Id $process.Id -Force -ErrorAction Stop
    }
}

function Capture-State {
    param([Parameter(Mandatory = $true)][string]$Label)

    Write-Log "Capturing $Label state."
    $deviceResult = Invoke-PnpUtil -ArgumentList @('/enum-devices', '/deviceid', $hardwareId, '/deviceids', '/drivers')
    $interfaceResult = Invoke-PnpUtil -ArgumentList @('/enum-interfaces', '/class', $controlInterfaceClassGuid, '/properties')
    $probeResult = Invoke-ProbeCheck

    return [pscustomobject]@{
        Device = $deviceResult
        Interface = $interfaceResult
        Probe = $probeResult
    }
}

function Get-DeviceInstanceId {
    param([Parameter(Mandatory = $true)]$DeviceResult)

    foreach ($line in ([string]$DeviceResult.Output -split "`r?`n")) {
        if ($line -match '^\s*Instance ID\s*:\s*(.+?)\s*$') {
            return $Matches[1].Trim()
        }
    }

    return $null
}

function Get-DeviceFailureLines {
    param([string]$PnpOutput)

    $failureLines = @()
    foreach ($line in ($PnpOutput -split "`r?`n")) {
        if ($line -match '^\s*(Problem|Problem Code|Status)\s*:\s*(.+?)\s*$') {
            $trimmed = $line.Trim()
            if ($trimmed -notmatch '(?i)\b(started|none|0|0x0|CM_PROB_NONE)\b') {
                $failureLines += $trimmed
            }
        }
    }

    return $failureLines
}

function Test-DeviceStarted {
    param([Parameter(Mandatory = $true)]$DeviceResult)

    if ($DeviceResult.ExitCode -ne 0) {
        Write-Log "Final PnP device query failed with exit code $($DeviceResult.ExitCode)."
        return $false
    }

    $output = [string]$DeviceResult.Output
    if ([string]::IsNullOrWhiteSpace($output)) {
        Write-Log 'Final PnP device query returned no output.'
        return $false
    }

    if ($output -notmatch [regex]::Escape($hardwareId)) {
        Write-Log "Final PnP device query did not list $hardwareId."
        return $false
    }

    $failureLines = @(Get-DeviceFailureLines -PnpOutput $output)
    if ($failureLines.Count -gt 0) {
        Write-Log "Final PnP state still reports a problem: $($failureLines -join '; ')"
        return $false
    }

    if ($output -match '(?im)^\s*Status\s*:\s*Started\s*$') {
        return $true
    }

    Write-Log 'Final PnP state did not include Status: Started.'
    return $false
}

function Invoke-ReviveSequence {
    param([Parameter(Mandatory = $true)]$BeforeState)

    if ($StopSunshineFirst) {
        Stop-SunshineForDriverRevive
    } else {
        Write-Log 'Leaving Sunshine service and processes running. Use -StopSunshineFirst to stop them before revive.'
    }

    $instanceId = Get-DeviceInstanceId -DeviceResult $BeforeState.Device
    if ([string]::IsNullOrWhiteSpace($instanceId)) {
        Write-Log "Unable to parse a device instance ID; falling back to hardware ID: $hardwareId"
        $restartArguments = @('/restart-device', '/deviceid', $hardwareId)
        $disableArguments = @('/disable-device', '/deviceid', $hardwareId, '/force')
        $enableArguments = @('/enable-device', '/deviceid', $hardwareId)
    } else {
        Write-Log "Using device instance ID for revive operations: $instanceId"
        $restartArguments = @('/restart-device', $instanceId)
        $disableArguments = @('/disable-device', $instanceId, '/force')
        $enableArguments = @('/enable-device', $instanceId)
    }

    Write-Log 'Attempting pnputil restart-device revive.'
    $restart = Invoke-PnpUtil -ArgumentList $restartArguments
    if ($restart.ExitCode -eq 0) {
        Write-Log 'pnputil restart-device completed successfully.'
    } elseif ($restart.ExitCode -eq 3010) {
        Write-Log 'pnputil restart-device requested a system reboot.'
    } else {
        Write-Log "pnputil restart-device failed with exit code $($restart.ExitCode)."
    }

    Write-Log 'Scanning devices after revive attempt.'
    $scan = Invoke-PnpUtil -ArgumentList @('/scan-devices')
    if ($scan.ExitCode -ne 0) {
        Write-Log "pnputil scan-devices failed with exit code $($scan.ExitCode)."
    }

    $retryProbe = Invoke-ProbeCheck
    if ($SkipProbe -or ($null -ne $retryProbe -and $retryProbe.ExitCode -eq 0)) {
        Write-Log 'Temporary display self-test passed after pnputil restart-device.'
        return
    }

    Write-Log 'Attempting pnputil disable-device/enable-device revive.'
    $disable = Invoke-PnpUtil -ArgumentList $disableArguments
    if ($disable.ExitCode -ne 0) {
        Write-Log "pnputil disable-device failed with exit code $($disable.ExitCode); continuing to enable attempt."
    }

    $enable = Invoke-PnpUtil -ArgumentList $enableArguments
    if ($enable.ExitCode -ne 0) {
        Write-Log "pnputil enable-device failed with exit code $($enable.ExitCode)."
    }

    Write-Log 'Scanning devices after disable/enable revive attempt.'
    $scan = Invoke-PnpUtil -ArgumentList @('/scan-devices')
    if ($scan.ExitCode -ne 0) {
        Write-Log "pnputil scan-devices failed with exit code $($scan.ExitCode)."
    }
}

if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = New-DefaultLogPath
}
$script:LogPath = $LogPath
New-Item -ItemType Directory -Path (Split-Path -Parent $script:LogPath) -Force | Out-Null

if (-not (Test-Administrator)) {
    Write-Log 'Administrator privileges are required; relaunching elevated.'
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-LogPath', $script:LogPath)
    if ($StopSunshineFirst) {
        $arguments += '-StopSunshineFirst'
    }
    if ($SkipProbe) {
        $arguments += '-SkipProbe'
    }

    $process = Start-Process -FilePath 'powershell.exe' -ArgumentList (ConvertTo-ProcessArgumentString -ArgumentList $arguments) -Verb RunAs -PassThru
    if ($null -ne $process) {
        $process.WaitForExit()
        exit $process.ExitCode
    }

    exit 1
}

$script:PnpUtilPath = Resolve-SystemToolPath -ToolName 'pnputil.exe'

Write-Log 'Sunshine virtual display revive test started.'
Write-Log "Log file: $script:LogPath"
Write-Log "pnputil path: $script:PnpUtilPath"
Write-Log "probe path: $probePath"

$exitCode = 1
try {
    $before = Capture-State -Label 'before'
    if (-not $SkipProbe -and $null -ne $before.Probe -and $before.Probe.ExitCode -eq 0) {
        Write-Log 'Initial temporary display self-test passed; no revive is required.'
        $exitCode = 0
    } else {
        Invoke-ReviveSequence -BeforeState $before
    }
    $after = Capture-State -Label 'after'

    $deviceStarted = Test-DeviceStarted -DeviceResult $after.Device
    $probePassed = $SkipProbe -or ($null -ne $after.Probe -and $after.Probe.ExitCode -eq 0)

    if ((-not $SkipProbe) -and $probePassed) {
        Write-Log 'Temporary display self-test passed; revive test succeeded.'
        $exitCode = 0
    } elseif ($SkipProbe -and $deviceStarted) {
        Write-Log 'Final PnP state is Started; revive test succeeded with probe skipped.'
        $exitCode = 0
    } elseif ($deviceStarted -and -not $probePassed) {
        Write-Log 'VIRTUAL_DISPLAY_RESTART_REQUIRED: Virtual display driver installed, but Windows restart is required before virtual display can function.'
        Write-Log 'Final PnP state is Started, but temporary display self-test still failed; revive test failed.'
        $exitCode = 3
    } else {
        Write-Log 'VIRTUAL_DISPLAY_RESTART_REQUIRED: Virtual display driver installed, but Windows restart is required before virtual display can function.'
        Write-Log 'Final PnP state is not Started; revive test failed.'
        $exitCode = 2
    }
} catch {
    Write-Log "Fatal error: $($_.Exception.Message)"
    $exitCode = 1
} finally {
    Write-Log "Sunshine virtual display revive test finished with exit code $exitCode."
}

exit $exitCode
