param([string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path)
$ErrorActionPreference = 'Stop'
function Assert-That([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Get-FailureMessage([scriptblock]$Action) {
    try { $null = & $Action } catch { return [string]$_.Exception.Message }
    throw 'Expected the operation to fail closed.'
}
function Assert-Stages([string]$Text, [string[]]$Expected) {
    $records = @([regex]::Matches($Text, 'DRIVER_INSTALL_SAFETY_STAGE:([a-z-]+):([0-9]+)'))
    Assert-That ($records.Count -eq $Expected.Count) "Expected $($Expected.Count) inventory stages, got $($records.Count): $Text"
    $previous = -1L
    for ($i = 0; $i -lt $records.Count; $i++) {
        Assert-That ($records[$i].Groups[1].Value -ceq $Expected[$i]) "Unexpected inventory stage order: $Text"
        $elapsed = [long]$records[$i].Groups[2].Value
        Assert-That ($elapsed -ge $previous) "Inventory elapsed milliseconds must be monotonic: $Text"
        $previous = $elapsed
    }
}
function Assert-ChildStopped {
    Assert-That (Test-Path -LiteralPath $script:childPidPath -PathType Leaf) 'The fixture child must have started before its deadline'
    $childPid = [int](Get-Content -LiteralPath $script:childPidPath -Raw)
    Assert-That ($null -eq (Get-Process -Id $childPid -ErrorAction SilentlyContinue)) 'The inventory child must be gone before the guard continues'
}

# These are the only Windows inventory boundaries replaced. The actual generated
# query and its process launch, pipe capture, deadline, and termination all run.
$fixtureProviders = @'
function Write-FixtureNoise {
    [Console]::Out.WriteLine('UNRELATED_STDOUT_SECRET')
    [Console]::Error.WriteLine('UNRELATED_STDERR_SECRET')
    [Console]::Out.WriteLine('DRIVER_INSTALL_SAFETY_STAGE:unknown:123')
    [Console]::Out.WriteLine('DRIVER_INSTALL_SAFETY_STAGE:started:-1')
    [Console]::Out.WriteLine('DRIVER_INSTALL_SAFETY_STAGE:started:1234567890123')
    [Console]::Out.WriteLine('DRIVER_INSTALL_SAFETY_STAGE:started:12 private-path')
    [Console]::Out.WriteLine('prefix DRIVER_INSTALL_SAFETY_STAGE:started:12')
    [Console]::Out.WriteLine('driver_install_safety_stage:started:12')
    [Console]::Error.WriteLine('DRIVER_INSTALL_SAFETY_STAGE:devices-done:999')
}
function Get-WindowsDriver {
    param([switch]$Online, [switch]$All, $ErrorAction)
    if (!$Online -or $All) { throw 'Display inventory must exclude inbox driver enumeration.' }
    if ($FixtureCase -eq 'packages-hang') { Write-FixtureNoise; Start-Sleep -Seconds 45 }
    if ($FixtureCase -eq 'packages-failure') { throw 'FIXTURE_PACKAGE_PROVIDER_FAILURE' }
    if ($FixtureCase -eq 'success') { [Console]::Out.WriteLine('UNRELATED_SUCCESS_OUTPUT') }
    return @($state.Packages)
}
function Get-CimInstance {
    param($ClassName, $ErrorAction)
    if ($ClassName -ne 'Win32_PnPSignedDriver') { throw 'Unexpected inventory provider.' }
    if ($FixtureCase -eq 'devices-hang') { Write-FixtureNoise; Start-Sleep -Seconds 45 }
    if ($FixtureCase -eq 'devices-failure') { throw 'FIXTURE_CIM_PROVIDER_FAILURE' }
    return @($state.Devices)
}
'@
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('display-inventory-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    foreach ($driver in @('sunshine', 'sudovda')) {
        $tokens = $null; $errors = $null
        $path = Join-Path $RepositoryRoot "src_assets/windows/drivers/$driver/install.ps1"
        $ast = [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
        Assert-That (!$errors.Count) "$driver installer must parse"
        $functions = @('ConvertTo-DriverComponentVersion', 'Get-InfComponentVersion', 'Test-SafeDriverInstall',
            'Get-DriverInstallSafetyInventory', 'ConvertFrom-DriverInstallSafetyInventoryResult',
            'Invoke-DriverInstallSafetyQuery', 'Invoke-DriverInstallSafetyCommand', 'ConvertTo-ProcessArgumentString')
        $functions += if ($driver -eq 'sunshine') { 'Invoke-DriverProcessCapture' } else { 'Invoke-Process' }
        foreach ($name in $functions) {
            $node = $ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name}.GetNewClosure(), $true)
            Assert-That ($null -ne $node) "$driver is missing $name"
            . ([scriptblock]::Create($node.Extent.Text))
        }
        $scriptDir = $scratch
        $script:realSafetyQuery = ${function:Invoke-DriverInstallSafetyQuery}
        $script:fixtureCase = 'success'
        $script:fixtureTimeout = 15
        $storeInf = Join-Path $scratch 'component.inf'
        $script:fixtureState = [pscustomobject]@{
            Packages = @(
                [pscustomobject]@{ OriginalFileName=$storeInf; Driver='oem1.inf' },
                [pscustomobject]@{ OriginalFileName=(Join-Path $scratch 'other.inf'); Driver='oem2.inf' }
            )
            Devices = @(
                [pscustomobject]@{ DeviceID='root\test\0000'; HardwareID='OTHER'; DriverVersion='1.2.0.11' },
                [pscustomobject]@{ DeviceID='ROOT\ELSEWHERE\0000'; HardwareID='root\test'; DriverVersion='1.2.0.12' },
                [pscustomobject]@{ DeviceID='PCI\OTHER'; HardwareID='ROOT\TEST-NOT-EXACT'; DriverVersion='9.9.9.9' }
            )
        }
        function Invoke-DriverInstallSafetyQuery {
            param($ScriptText, $TimeoutSeconds)
            $script:observedTimeout = $TimeoutSeconds
            $script:queryCalls++
            $script:childPidPath = Join-Path $scratch ('child-' + [guid]::NewGuid().ToString('N') + '.pid')
            $encodedState = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes((ConvertTo-Json $script:fixtureState -Depth 6 -Compress)))
            $prelude = "`$FixtureCase='$script:fixtureCase'; " +
                "`$state=ConvertFrom-Json ([Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('$encodedState'))); " +
                "[IO.File]::WriteAllText('$($script:childPidPath.Replace("'", "''"))',[string]`$PID);`n" + $fixtureProviders + "`n"
            if ($script:fixtureCase -eq 'startup-hang') {
                # Simulate a child blocked before the query can report its first stage.
                $prelude += "Write-FixtureNoise; Start-Sleep -Seconds 45`n"
            }
            $timer = [Diagnostics.Stopwatch]::StartNew()
            try {
                $script:lastQueryOutput = & $script:realSafetyQuery -ScriptText ($prelude + $ScriptText) -TimeoutSeconds $script:fixtureTimeout
                return $script:lastQueryOutput
            } finally {
                $timer.Stop()
                Assert-That ($timer.Elapsed.TotalSeconds -lt 12) 'Inventory fixtures must return within a bounded wall-clock interval'
            }
        }

        $script:queryCalls = 0
        $inventory = Get-DriverInstallSafetyInventory -InfName 'COMPONENT.INF' -HardwareId 'ROOT\TEST'
        Assert-That ($script:observedTimeout -eq 15) "$driver must retain its default 15-second inventory deadline"
        Assert-That ($inventory.Packages.Count -eq 1 -and $inventory.Packages[0].Driver -eq 'oem1.inf') "$driver must retain only matching Driver Store packages"
        Assert-That ($inventory.Devices.Count -eq 2 -and $inventory.Devices[0].DriverVersion -eq '1.2.0.11' -and $inventory.Devices[1].DriverVersion -eq '1.2.0.12') "$driver must match active DeviceID prefixes or exact HardwareID values, ignoring case"
        Assert-Stages $script:lastQueryOutput @('started', 'packages-start', 'packages-done', 'devices-start', 'devices-done')
        Assert-That ($script:lastQueryOutput.Contains('UNRELATED_SUCCESS_OUTPUT')) 'The result decoder must tolerate real unrelated provider output'
        Assert-ChildStopped

        $script:fixtureTimeout = 2
        foreach ($case in @('packages-hang', 'devices-hang', 'startup-hang')) {
            $script:fixtureCase = $case
            $message = Get-FailureMessage { Get-DriverInstallSafetyInventory -InfName 'component.inf' -HardwareId 'ROOT\TEST' }
            Assert-That ($script:observedTimeout -eq 15) "$driver $case must originate with the production default deadline"
            Assert-That ($message.Contains('timed out after 2 seconds') -and $message.Contains('driver unchanged')) "$driver $case must fail closed at the shortened test deadline: $message"
            $expected = switch ($case) {
                'packages-hang' { @('started', 'packages-start') }
                'devices-hang' { @('started', 'packages-start', 'packages-done', 'devices-start') }
                'startup-hang' { @() }
            }
            Assert-Stages $message @($expected)
            Assert-That ($message.Contains('No inventory stage reported.') -eq ($case -eq 'startup-hang')) "$driver $case must distinguish missing stages from provider hangs"
            foreach ($noise in @('UNRELATED_', 'unknown:123', 'started:-1', '1234567890123', 'private-path', 'prefix ', 'driver_install_safety_stage:', 'devices-done:999')) {
                Assert-That (!$message.Contains($noise)) "$driver $case leaked unrelated or malformed output: $message"
            }
            Assert-That (!$message.Contains('DRIVER_INSTALL_SAFETY_INVENTORY:')) 'Timeout diagnostics must not include an inventory payload'
            Assert-ChildStopped
        }

        # A stream of otherwise valid records must also remain bounded. This is
        # real child output, not a mocked timeout exception or a source assertion.
        $script:fixtureCase = 'stage-limit'
        $stageLimitScript = @'
foreach ($n in 1..8) { [Console]::Out.WriteLine('DRIVER_INSTALL_SAFETY_STAGE:started:' + $n) }
Start-Sleep -Seconds 45
'@
        $message = Get-FailureMessage { Invoke-DriverInstallSafetyQuery -ScriptText $stageLimitScript -TimeoutSeconds 15 }
        Assert-Stages $message @('started', 'started', 'started', 'started', 'started')
        foreach ($n in 1..3) { Assert-That (!$message.Contains("DRIVER_INSTALL_SAFETY_STAGE:started:$n")) 'Only the last five valid stage records may be included' }
        foreach ($n in 4..8) { Assert-That ($message.Contains("DRIVER_INSTALL_SAFETY_STAGE:started:$n")) 'The most recent valid stage records must be preserved' }
        Assert-ChildStopped

        $script:fixtureTimeout = 15
        foreach ($case in @('packages-failure', 'devices-failure')) {
            $script:fixtureCase = $case
            $message = Get-FailureMessage { Get-DriverInstallSafetyInventory -InfName 'component.inf' -HardwareId 'ROOT\TEST' }
            $providerError = if ($case -eq 'packages-failure') { 'FIXTURE_PACKAGE_PROVIDER_FAILURE' } else { 'FIXTURE_CIM_PROVIDER_FAILURE' }
            Assert-That ($message.Contains('failed with exit code 1') -and $message.Contains($providerError) -and $message.Contains('driver unchanged')) "$driver $case must preserve its actual provider failure: $message"
            Assert-That (!$message.Contains('timed out')) 'Provider failure must not be mislabeled as a timeout'
            Assert-ChildStopped
        }

        # Exercise source provenance through the real guard, INF reader, and
        # version parser. Only catalog verification is replaced, and no install
        # body, driver mutation, trust change, or service operation is executed.
        $targetInf = Join-Path $scratch 'bundled.inf'
        $retainedInf = Join-Path $scratch 'retained.inf'
        $script:signatureChecks = 0
        function Assert-ExpectedDisplayPackage {
            param($InfPath, $CatalogPath, $HardwareId)
            $script:signatureChecks++
            return $true
        }
        function Invoke-FixtureDecision {
            Test-SafeDriverInstall -InfPath $targetInf -CatalogPath 'fixture.cat' -HardwareId 'ROOT\TEST'
        }
        foreach ($source in @('bundled INF', 'active device', 'retained INF')) {
            $script:fixtureCase = 'version-provenance'
            "[Version]`nDriverVer=01/01/2000,1.4.5.0" | Set-Content -LiteralPath $targetInf
            "[Version]`nDriverVer=01/01/2000,1.4.3.0" | Set-Content -LiteralPath $retainedInf
            $script:fixtureState = [pscustomobject]@{ Packages=@(); Devices=@() }
            $beforeQueries = $script:queryCalls
            switch ($source) {
                'bundled INF' { "[Version]`nDriverVer=01/01/2000,1.4.4." | Set-Content -LiteralPath $targetInf }
                'active device' { $script:fixtureState.Devices = @([pscustomobject]@{ DeviceID='ROOT\TEST\0000'; HardwareID='ROOT\TEST'; DriverVersion='1.4.4.' }) }
                'retained INF' {
                    "[Version]`nDriverVer=01/01/2000,1.4.4." | Set-Content -LiteralPath $retainedInf
                    # The published inventory filename must match the requested
                    # bundled INF while its contents remain a real local file.
                    $retainedDir = Join-Path $scratch 'retained'
                    New-Item -ItemType Directory -Path $retainedDir -Force | Out-Null
                    $retainedInf = Join-Path $retainedDir 'bundled.inf'
                    "[Version]`nDriverVer=01/01/2000,1.4.4." | Set-Content -LiteralPath $retainedInf
                    $script:fixtureState.Packages = @([pscustomobject]@{ OriginalFileName=$retainedInf; Driver='oem1.inf' })
                }
            }
            $message = Get-FailureMessage { Invoke-FixtureDecision }
            Assert-That ($message.Contains("Unknown driver component version from ${source}:") -and $message.Contains("'1.4.4.'") -and $message.Contains('Driver unchanged.')) "$driver must identify $source while rejecting the unchanged malformed version: $message"
            foreach ($privatePath in @($scratch, $targetInf, $retainedInf)) {
                Assert-That (!$message.Contains($privatePath)) "$driver malformed-version provenance must not expose local paths: $message"
            }
            Assert-That ($script:signatureChecks -eq 0) 'Malformed versions must stop before catalog validation'
            if ($source -eq 'bundled INF') {
                Assert-That ($script:queryCalls -eq $beforeQueries) 'Malformed bundled versions must stop before inventory'
            } else {
                Assert-That ($script:queryCalls -eq ($beforeQueries + 1)) 'Installed-version provenance must use the actual inventory result'
                Assert-ChildStopped
            }
        }
        Assert-That ((ConvertTo-DriverComponentVersion '1.4.4.0') -eq [version]'1.4.4.0') 'Valid four-part versions must retain optional-source compatibility'
        $message = Get-FailureMessage { ConvertTo-DriverComponentVersion '1.4.65536.0' -Source 'fixture source' }
        Assert-That ($message.Contains('Invalid driver component version from fixture source:') -and $message.Contains('Driver unchanged.')) 'Out-of-range component versions must also retain source provenance'
        $message = Get-FailureMessage { ConvertTo-DriverComponentVersion '1.4.4.' }
        Assert-That ($message.Contains('driver metadata') -and $message.Contains("'1.4.4.'")) 'Legacy callers must still reject malformed versions with a useful default source'
        "[Version]`nDriverVer=01/01/2000,1.4.4.0" | Set-Content -LiteralPath $targetInf
        Assert-That ((Get-InfComponentVersion $targetInf) -eq [version]'1.4.4.0') 'Valid INF versions must retain optional-source compatibility'
        Write-Output "$driver inventory stage, timeout, provider failure, filtering, and version provenance behavior passed."
    }
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
