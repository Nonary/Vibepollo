param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path,
    [string]$InstallerPath = (Join-Path $RepositoryRoot 'src_assets/windows/drivers/sunshine/install.ps1')
)
$ErrorActionPreference = 'Stop'
# Parse, but never dot-source or launch, the installer. Only explicitly selected
# production functions and its install control flow enter the mock child process.
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($InstallerPath, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw "Installer parse failed: $($errors.Message -join '; ')" }
$serviceBrand = if ($ast.EndBlock.Extent.Text.Contains('function Stop-ApolloForDriverInstall')) { 'Apollo' } else { 'Sunshine' }
$names = @(
    'ConvertTo-ProcessArgumentString', 'Invoke-DriverProcess', 'Write-DriverRestartRequired',
    "Stop-${serviceBrand}ForDriverInstall", "Start-${serviceBrand}ServiceIfNeeded",
    'Stop-VirtualDisplayBrokerForDriverInstall', 'Start-VirtualDisplayBrokerIfNeeded',
    'Stop-SunshineVirtualDisplayDriverHost', 'Restart-SunshineVirtualDisplayRuntime',
    'Install-DriverPackage', 'Remove-DriverPackage', 'Remove-DeviceNodeForHardwareId', 'Remove-DeviceNode',
    'Test-TemporaryVirtualDisplay', 'Reset-SunshineVirtualDisplayDeviceNode', 'Invoke-InstallerHealthCheck',
    'Install-SunshineVirtualDisplayDriver'
)
$functions = @($ast.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -in $names
})
$trapText = ($ast.EndBlock.Traps | ForEach-Object { $_.Extent.Text }) -join "`n"
$statements = @($ast.EndBlock.Statements)
$start = @($statements | Where-Object { $_.Extent.Text -eq 'Install-ExpectedDisplayTrust' })
if ($start.Count -ne 1) { throw 'Ambiguous install entry point' }
$installBody = ($statements | Where-Object { $_.Extent.StartOffset -ge $start[0].Extent.StartOffset } |
    ForEach-Object { $_.Extent.Text }) -join "`n"
$production = (($functions | ForEach-Object { $_.Extent.Text }) -join "`n") + "`n" + $trapText + "`n" + $installBody
# Fail closed if production grows a new command boundary. All commands capable of
# touching the host below are replaced by fixture functions before execution.
$boundaryNames = @(
    'Start-Process', 'Get-Service', 'Start-Service', 'Stop-Service', 'Get-Process',
    'Stop-Process', 'Get-CimInstance', 'Test-Path', 'Start-Sleep', 'Invoke-DriverHealthProcess',
    'Install-ExpectedDisplayTrust', 'Undo-DisplayTrustProvisioning', 'Register-VulkanLayer',
    'Test-DriverPackageRefreshNeeded', 'Test-DeviceNodePresent', 'Wait-DriverStoreMatchesPackagedPayload',
    'Write-DriverStorePayloadDiagnostics', 'Initialize-DriverStateRegistryAccess',
    'Get-SunshineDeviceInstanceId', 'Get-SunshineDriverPublishedNames'
)
$allowed = $names + $boundaryNames + @('Write-Host', 'Write-Warning', 'Where-Object')
$parsed = [Management.Automation.Language.Parser]::ParseInput($production, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw "Extracted control flow parse failed: $($errors.Message -join '; ')" }
foreach ($command in $parsed.FindAll({ param($node) $node -is [Management.Automation.Language.CommandAst] }, $true)) {
    if ($command.GetCommandName() -notin $allowed) { throw "Unmocked production command: $($command.Extent.Text)" }
}

$mocks = @'
function Record([string]$Event) { [void]$script:events.Add($Event) }
function Code([string]$Kind) {
    if (!$script:counts.ContainsKey($Kind)) { $script:counts[$Kind] = 0 }
    $script:counts[$Kind]++
    $key = $Kind + ':' + $script:counts[$Kind]
    Record $key
    if ($key -eq $config.RebootAt) { return 3010 }
    if ($key -eq $config.FaultAt) { return 5 }
    return 0
}
function Start-Process {
    param($FilePath, $ArgumentList, $WorkingDirectory, [switch]$PassThru, $WindowStyle)
    $kind = switch -Regex ($ArgumentList) {
        '/add-driver' { if ($ArgumentList -match '/install') { 'install' } else { 'stage' }; break }
        '/delete-driver' { 'delete'; break }
        '--remove-device-node' { 'remove'; break }
        '--create-device-node' { 'create'; break }
        '/scan-devices' { 'scan'; break }
        default { throw "Unexpected native command: $ArgumentList" }
    }
    $code = Code $kind
    $process = [pscustomobject]@{ ExitCode = $code; TimedOut = ($kind + ':' + $script:counts[$kind]) -eq $config.TimeoutAt }
    $process | Add-Member ScriptMethod WaitForExit { param($Milliseconds) return !$this.TimedOut }
    $process | Add-Member ScriptMethod Kill { Record 'kill-timeout' }
    return $process
}
function Invoke-DriverHealthProcess {
    param($FilePath, $ArgumentList, $TimeoutSeconds)
    $kind = switch ($ArgumentList[0]) {
        '--self-test-temp' { 'probe' }
        '/restart-device' { 'restart' }
        '/disable-device' { 'disable' }
        '/enable-device' { 'enable' }
        default { throw "Unexpected health command: $ArgumentList" }
    }
    $code = Code $kind
    if ($kind -eq 'probe') {
        $index = [Math]::Min($script:counts.probe - 1, $config.Probes.Count - 1)
        $code = if ($config.Probes[$index]) { 0 } else { 1 }
    }
    return [pscustomobject]@{ ExitCode = $code; Output = 'mock boundary result' }
}
function Get-Service { param($Name, $ErrorAction) return $script:services[$Name] }
function Stop-Service { param($Name, [switch]$Force, $ErrorAction) Record "stop/$Name"; $script:services[$Name].Status = 'Stopped' }
function Start-Service {
    param($Name, $ErrorAction)
    if ((Code "start/$Name") -ne 0) { throw 'mock service start failure' }
    $script:services[$Name].Status = 'Running'
}
function Get-Process { param($Name, $ErrorAction) }
function Stop-Process { param($Id, [switch]$Force, $ErrorAction) Record 'stop/umdf' }
function Get-CimInstance {
    param($ClassName, $Filter, $ErrorAction)
    return [pscustomobject]@{ CommandLine = '-DeviceGroupId:mock-group'; ProcessId = 99 }
}
function Test-Path { param($LiteralPath, $PathType) return $true }
function Start-Sleep { param($Milliseconds) Record 'settle' }
function Install-ExpectedDisplayTrust { Record 'trust' }
function Undo-DisplayTrustProvisioning { Record 'undo-trust' }
function Register-VulkanLayer { Record 'vulkan' }
function Test-DriverPackageRefreshNeeded { Record 'refresh-query'; return $config.Refresh }
function Test-DeviceNodePresent { Record 'node-query'; return $config.Node }
function Wait-DriverStoreMatchesPackagedPayload {
    param($TimeoutSeconds)
    [void](Code 'payload-query')
    return $script:counts.'payload-query' -gt $config.StaleQueries
}
function Write-DriverStorePayloadDiagnostics { Record 'payload-diagnostics' }
function Initialize-DriverStateRegistryAccess { Record 'registry' }
function Get-SunshineDeviceInstanceId { Record 'instance-query'; return $config.Instance }
function Get-SunshineDriverPublishedNames { Record 'packages-query'; return @('oem1.inf', 'oem2.inf') }
'@

$initialization = @'
$ErrorActionPreference = 'Stop'
$config = Get-Content -LiteralPath $args[0] -Raw | ConvertFrom-Json
$script:events = New-Object 'System.Collections.Generic.List[string]'
$script:counts = @{}
$script:services = @{}
foreach ($name in @('SunshineService', 'ApolloService', 'SunshineVirtualDisplayBroker')) {
    $service = [pscustomobject]@{ Status = 'Running' }
    $service | Add-Member ScriptMethod WaitForStatus { param($Status, $Timeout) if ($this.Status -ne $Status) { throw 'mock service did not transition' } }
    $script:services[$name] = $service
}
$script:rebootRequired = $config.Pending
$script:sunshineServiceWasRunning = $false
$script:apolloServiceWasRunning = $false
$script:virtualDisplayBrokerWasRunning = $false
$scriptDir = 'mock'; $pnputil = 'mock-pnputil'; $nefConc = 'mock-nefconc'
$infPath = 'mock.inf'; $probePath = 'mock-probe'; $hardwareId = 'ROOT\MOCK'; $classGuid = 'mock'; $deviceGroupId = 'mock-group'
$Uninstall = $config.Uninstall; $InstallerBestEffort = $config.BestEffort
'@
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('display-reboot-' + [guid]::NewGuid().ToString('N'))
$hostExe = (Get-Process -Id $PID).Path
New-Item -ItemType Directory -Path $scratch | Out-Null
$script:cases = 0
try {
    # Keep the real trap in its own scope, so its service restoration completes
    # before the outer finally records state, including on exit 0 or strict throw.
    $child = $initialization + "`ntry { & {`n" +
        (($functions | ForEach-Object { $_.Extent.Text }) -join "`n") + "`n" + $mocks + "`n" + $trapText + @'

switch ($config.Entry) {
    'health' { Invoke-InstallerHealthCheck; return }
    'reset' { $null = Reset-SunshineVirtualDisplayDeviceNode; return }
    'runtime' { Restart-SunshineVirtualDisplayRuntime; return }
    'packages' { Remove-DriverPackage; return }
}
'@ + "`n" + $installBody + @'

} } finally {
    [pscustomobject]@{
        Events = @($script:events.ToArray()); Reboot = $script:rebootRequired
        HostStatus = $script:services[($config.Brand + 'Service')].Status
        BrokerStatus = $script:services.SunshineVirtualDisplayBroker.Status
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $args[1]
}
'@
    $childPath = Join-Path $scratch 'mock-control-flow.ps1'
    Set-Content -LiteralPath $childPath -Value $child -Encoding UTF8
    function Run-Case {
        param([string]$Name, [hashtable]$Options = @{})
        $config = @{
            RebootAt = ''; FaultAt = ''; TimeoutAt = ''; Pending = $false; BestEffort = $false; Uninstall = $false; Entry = 'install'
            Probes = @($true); Refresh = $true; Node = $true; StaleQueries = 0; Instance = 'ROOT\MOCK\0000'; Brand = $serviceBrand
        }
        foreach ($key in $Options.Keys) { $config[$key] = $Options[$key] }
        $configPath = Join-Path $scratch 'case.json'; $resultPath = Join-Path $scratch 'result.json'
        $config | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $configPath
        if (Test-Path $resultPath) { Remove-Item -LiteralPath $resultPath }
        # Windows PowerShell reports stderr as error records in the parent; keep
        # the child's actual exit code authoritative for strict failure cases.
        $savedPreference = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        try {
            $output = (& $hostExe -NoLogo -NoProfile -NonInteractive -File $childPath $configPath $resultPath 2>&1 | Out-String)
            $code = $LASTEXITCODE
        } finally { $ErrorActionPreference = $savedPreference }
        if (!(Test-Path $resultPath)) { throw "$Name did not record fixture state: $output" }
        $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
        $script:cases++
        return [pscustomobject]@{ Name = $Name; Code = $code; Output = $output; Events = @($result.Events); Reboot = $result.Reboot; HostStatus = $result.HostStatus; BrokerStatus = $result.BrokerStatus }
    }
    function Check($Result, [bool]$Condition, [string]$Message) {
        if (!$Condition) { throw "$($Result.Name): $Message`n$($Result.Events -join ', ')`n$($Result.Output)" }
    }
    function Check-Reboot($Result, [string]$LastMutation) {
        Check $Result ($Result.Code -eq 0 -and $Result.Reboot) 'Reboot result must remain a successful pending install'
        Check $Result ($Result.Output.Contains('VIRTUAL_DISPLAY_RESTART_REQUIRED')) 'Bootstrapper restart marker is missing'
        Check $Result ($Result.HostStatus -eq 'Running' -and $Result.BrokerStatus -eq 'Running') 'Previously running services must be restored'
        $mutations = @($Result.Events | Where-Object { $_ -match '^(stage|install|delete|remove|create|scan|restart|disable|enable):|^stop/umdf$|^probe:' })
        Check $Result ($mutations[-1] -eq $LastMutation) 'Driver mutation or old-driver probe ran after reboot result'
    }

    $normal = Run-Case 'normal'
    Check $normal ($normal.Code -eq 0 -and !$normal.Reboot -and $normal.Events -contains 'probe:1') 'Normal install must reach health check without a reboot warning'
    Check $normal ([Array]::IndexOf($normal.Events, 'stage:1') -lt [Array]::IndexOf($normal.Events, ('stop/' + $serviceBrand + 'Service'))) 'Services stopped before package staging'
    Check $normal ($normal.HostStatus -eq 'Running' -and $normal.BrokerStatus -eq 'Running') 'Normal install did not restore services'
    Check $normal (!$normal.Output.Contains('VIRTUAL_DISPLAY_RESTART_REQUIRED')) 'Healthy install produced a reboot marker'
    foreach ($entry in @('health', 'reset', 'runtime', 'packages')) {
        $pending = Run-Case "sticky entry/$entry" @{ Entry = $entry; Pending = $true }
        Check $pending ($pending.Code -eq 0 -and $pending.Reboot -and $pending.Events.Count -eq 0) 'A pending reboot allowed repair or was cleared by an old-driver probe'
    }
    $uninstall = Run-Case 'explicit uninstall package removal preserved' @{ Entry = 'packages'; Pending = $true; Uninstall = $true }
    Check $uninstall (@($uninstall.Events | Where-Object { $_ -match '^delete:' }).Count -eq 4) 'Install deferral changed explicit uninstall package cleanup'
    # A stage-only 3010 and an install 3010 both take precedence over a healthy
    # old driver and over an unchanged/mixed on-disk DriverStore observation.
    foreach ($refresh in @($false, $true)) {
        foreach ($point in @('stage:1', 'install:1')) {
            if (!$refresh -and $point -eq 'install:1') { continue }
            Check-Reboot (Run-Case "old-driver/$refresh/$point" @{ RebootAt = $point; Refresh = $refresh; StaleQueries = 9 }) $point
        }
    }
    Check-Reboot (Run-Case 'create requires reboot' @{ Node = $false; RebootAt = 'create:1' }) 'create:1'
    Check-Reboot (Run-Case 'bind new node requires reboot' @{ Node = $false; RebootAt = 'install:2' }) 'install:2'
    foreach ($point in @('remove:1', 'delete:1', 'delete:2', 'delete:3', 'install:2', 'scan:1', 'scan:2')) {
        Check-Reboot (Run-Case "restage/$point" @{ RebootAt = $point; StaleQueries = 1 }) $point
    }
    $restage = Run-Case 'non-reboot removal retry' @{ StaleQueries = 1 }
    Check $restage ($restage.Code -eq 0 -and !$restage.Reboot -and @($restage.Events | Where-Object { $_ -match '^delete:' }).Count -eq 4) 'Non-reboot package removal retry changed'
    # The equal payload/device route reaches the real health recovery sequence.
    foreach ($point in @('restart:1', 'scan:1', 'disable:1', 'enable:1', 'scan:2', 'scan:3', 'remove:1', 'delete:1', 'create:1', 'install:1', 'install:2', 'scan:4')) {
        Check-Reboot (Run-Case "health/$point" @{ Refresh = $false; RebootAt = $point; Probes = @($false) }) $point
    }
    foreach ($probes in @(@($false, $true), @($false, $false, $true), @($false, $false, $false, $true), @($false, $false, $false, $false, $true))) {
        $result = Run-Case "recovery stage $($probes.Count)" @{ Refresh = $false; Probes = $probes }
        Check $result ($result.Code -eq 0 -and !$result.Reboot) 'Non-reboot recovery should still succeed'
    }
    $unknown = Run-Case 'missing instance recovery' @{ Refresh = $false; Instance = ''; Probes = @($false, $true) }
    Check $unknown ($unknown.Code -eq 0 -and !$unknown.Reboot -and $unknown.Events -contains 'create:1') 'Missing instance should still allow non-reboot clean restaging'
    foreach ($point in @('restart:1', 'scan:1')) {
        $recovered = Run-Case "recoverable boundary failure/$point" @{ Refresh = $false; FaultAt = $point; Probes = @($false, $true) }
        Check $recovered ($recovered.Code -eq 0 -and !$recovered.Reboot) 'A failed revive step incorrectly blocked a later successful non-reboot probe'
    }
    $failedRecovery = Run-Case 'exhausted recovery' @{ Refresh = $false; Probes = @($false) }
    Check $failedRecovery ($failedRecovery.Code -eq 0 -and $failedRecovery.Reboot -and $failedRecovery.Output.Contains('VIRTUAL_DISPLAY_RESTART_REQUIRED')) 'Failed recovery must report restart required'
    foreach ($bestEffort in @($false, $true)) {
        foreach ($point in @('stage:1', 'install:1')) {
            $failed = Run-Case "failure/$bestEffort/$point" @{ FaultAt = $point; BestEffort = $bestEffort }
            Check $failed (($failed.Code -eq 0) -eq $bestEffort) 'Strict/optional failure policy changed'
            Check $failed (!$failed.Reboot -and !$failed.Output.Contains('VIRTUAL_DISPLAY_RESTART_REQUIRED')) 'Failure fabricated a native reboot result'
            Check $failed ($failed.HostStatus -eq 'Running' -and $failed.BrokerStatus -eq 'Running') 'Trap did not restore services'
            Check $failed (!($failed.Events | Where-Object { $_ -match '^(probe|delete|remove|create):' })) 'Install continued into repair after a native failure'
            if ($point -eq 'stage:1') { Check $failed ($failed.Events -contains 'undo-trust') 'Rejected staging did not undo provisioned trust' }
        }
    }
    $timeout = Run-Case 'native timeout' @{ TimeoutAt = 'install:1' }
    Check $timeout ($timeout.Code -ne 0 -and !$timeout.Reboot -and $timeout.Events -contains 'kill-timeout') 'Timeout must retain existing failure policy'
    $retry = Run-Case 'new invocation retries after failure'
    Check $retry ($retry.Code -eq 0 -and !$retry.Reboot) 'A previous failed invocation prevented a fresh attempt'
    $retryReboot = Run-Case 'new invocation still gets reboot' @{ RebootAt = 'install:1' }
    Check-Reboot $retryReboot 'install:1'
    $restoreFailure = Run-Case 'service restore failure preserves reboot' @{ RebootAt = 'install:1'; FaultAt = ('start/' + $serviceBrand + 'Service:1'); BestEffort = $true }
    Check $restoreFailure ($restoreFailure.Code -eq 0 -and $restoreFailure.Reboot -and $restoreFailure.Output.Contains('VIRTUAL_DISPLAY_RESTART_REQUIRED')) 'Trap erased reboot state after service restore failure'
    Write-Host "$script:cases production PowerShell control-flow fixtures passed. Native processes, services, devices, trust and registry operations were mocked."
} finally {
    # Delete only the files directly created by this fixture.
    foreach ($file in @('mock-control-flow.ps1', 'case.json', 'result.json')) {
        $path = Join-Path $scratch $file
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
    Remove-Item -LiteralPath $scratch
}
