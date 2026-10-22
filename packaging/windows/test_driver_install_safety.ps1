param([string]$RepositoryRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
$ErrorActionPreference = 'Stop'
function Assert-That([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$drivers = @('sunshine', 'sudovda', 'vhf-gamepad')
$baseline = $null
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('driver-safety-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    foreach ($driver in $drivers) {
        $path = Join-Path $RepositoryRoot "src_assets/windows/drivers/$driver/install.ps1"
        $tokens = $null; $errors = $null
        $ast = [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
        Assert-That ($errors.Count -eq 0) "Syntax error in $path"
        $text = Get-Content $path -Raw
        $block = [regex]::Match($text, '(?s)# DRIVER_INSTALL_SAFETY_BEGIN.*?# DRIVER_INSTALL_SAFETY_END').Value
        Assert-That ($block.Length -gt 0) "Missing safety policy: $driver"
        if ($driver -eq 'vhf-gamepad') {
            Assert-That ($block.Contains('$LocalTestPackage -and $AllowUntrustedLocalPackage -and') -and $block.Contains("@('NotTrusted', 'UnknownError')")) 'Only a signed local VHF candidate may defer trust for independent validation and recipient consent'
        } elseif ($null -eq $baseline) { $baseline = $block } else { Assert-That ($block -ceq $baseline) "Safety policy drift: $driver" }
        # Load only these pure/read-only functions, never the installer body.
        foreach ($name in @('ConvertTo-DriverComponentVersion', 'Get-InfComponentVersion', 'Test-SafeDriverInstall')) {
            $function = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }.GetNewClosure(), $true)
            . ([scriptblock]::Create($function.Extent.Text))
        }
        if ($driver -ne 'vhf-gamepad') {
            foreach ($name in @('ConvertTo-ProcessArgumentString', 'Invoke-DriverInstallSafetyQuery', 'ConvertFrom-DriverInstallSafetyInventoryResult', 'Get-DriverInstallSafetyInventory')) {
                $function = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }.GetNewClosure(), $true)
                . ([scriptblock]::Create($function.Extent.Text))
            }
            $processFunctions = if ($driver -eq 'sudovda') {
                @('Invoke-Process', 'Invoke-DriverInstallSafetyCommand')
            } else {
                @('Invoke-DriverProcessCapture', 'Invoke-DriverInstallSafetyCommand')
            }
            foreach ($name in $processFunctions) {
                $function = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }.GetNewClosure(), $true)
                . ([scriptblock]::Create($function.Extent.Text))
            }
        }
        $script:fakePackages = @(); $script:fakeDevices = @(); $script:queryFails = $false; $script:cimFails = $false
        $script:signatureChecks = 0
        $script:fakeSignature = [pscustomobject]@{ Status='Valid'; SignerCertificate=[pscustomobject]@{ Subject='CN=Publisher'; Issuer='CN=Public CA' } }
        function Get-WindowsDriver { param([switch]$Online,[switch]$All,$ErrorAction) if ($script:queryFails) { throw 'Mock query failure' }; return $script:fakePackages }
        function Get-CimInstance { param($ClassName,$ErrorAction) if ($script:cimFails) { throw 'Mock CIM failure' }; return $script:fakeDevices }
        if ($driver -ne 'vhf-gamepad') {
            function Get-DriverInstallSafetyInventory {
                param($InfName, $HardwareId, $TimeoutSeconds = 15)
                if ($script:queryFails) { throw 'Mock bounded inventory timeout' }
                if ($script:cimFails) { throw 'Mock bounded inventory provider failure' }
                return [pscustomobject]@{ Packages = @($script:fakePackages); Devices = @($script:fakeDevices) }
            }
        }
        function Get-AuthenticodeSignature { param($LiteralPath,$ErrorAction) return $script:fakeSignature }
        function Assert-ExpectedDisplayPackage {
            param($InfPath,$CatalogPath,$HardwareId)
            $script:signatureChecks++
            if (!$script:fakeSignature.SignerCertificate -or
                ($script:fakeSignature.Status -ne 'Valid' -and $script:fakeSignature.SignerCertificate.Subject -ne $script:fakeSignature.SignerCertificate.Issuer)) { throw 'Mock rejected package identity/integrity' }
            return $true
        }

        $targetInf = Join-Path $testRoot 'component.inf'
        $storeDir = Join-Path $testRoot 'store'; New-Item -ItemType Directory -Path $storeDir -Force | Out-Null
        $storeInf = Join-Path $storeDir 'component.inf'
        function Set-Target([string]$Version,[string]$Date='01/01/2000') { "[Version]`nDriverVer=$Date,$Version" | Set-Content $targetInf }
        function Set-Installed([string]$Version,[string]$Date='01/01/2099') {
            "[Version]`nDriverVer=$Date,$Version" | Set-Content $storeInf
            $script:fakePackages = @([pscustomobject]@{OriginalFileName=$storeInf;Driver='oem1.inf'})
            $script:fakeDevices = @([pscustomobject]@{DeviceID='ROOT\TEST\0000';DriverVersion=$Version})
        }
        function Invoke-Decision { Test-SafeDriverInstall -InfPath $targetInf -CatalogPath 'mock.cat' -HardwareId 'ROOT\TEST' }
        function Expect-Blocked([string]$Message) { $blocked=$false; try { $null=Invoke-Decision } catch { $blocked=$true }; Assert-That $blocked $Message }
        if ($driver -ne 'vhf-gamepad') {
            # Exercise each installer’s actual bounded process wrapper with harmless child
            # commands and the actual result decoder, without touching Windows
            # driver inventory, devices, packages, or trust stores.
            $scriptDir = $testRoot
            $probeOutput = Invoke-DriverInstallSafetyQuery -ScriptText "Write-Output 'bounded-query-ok'" -TimeoutSeconds 15
            $probeOutputText = [string]$probeOutput
            $probeOutputBase64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($probeOutputText))
            Assert-That ($probeOutput.Trim() -eq 'bounded-query-ok') "Bounded inventory child must return captured output (length: $($probeOutputText.Length); UTF-16 base64: $probeOutputBase64)"
            $failedChild = $false
            try { $null = Invoke-DriverInstallSafetyQuery -ScriptText 'exit 7' -TimeoutSeconds 15 } catch { $failedChild = $_.Exception.Message.Contains('exit code 7') }
            Assert-That $failedChild 'A failed inventory child must fail closed with its exit status'
            $timeoutPidPath = Join-Path $testRoot 'timed-out-inventory-child.pid'
            $escapedTimeoutPidPath = $timeoutPidPath.Replace("'", "''")
            $timeoutScript = "[IO.File]::WriteAllText('$escapedTimeoutPidPath',[string]`$PID); Start-Sleep -Seconds 45"
            $timedOutChild = $false
            try { $null = Invoke-DriverInstallSafetyQuery -ScriptText $timeoutScript -TimeoutSeconds 10 } catch { $timedOutChild = $_.Exception.Message.Contains('timed out after 10 seconds') }
            Assert-That $timedOutChild 'A blocked inventory child must be terminated after its deadline'
            Assert-That (Test-Path -LiteralPath $timeoutPidPath -PathType Leaf) 'Timed-out inventory child must start before the timeout fixture expires'
            $timedOutPid = [int](Get-Content -LiteralPath $timeoutPidPath -Raw -ErrorAction Stop)
            Assert-That ($null -eq (Get-Process -Id $timedOutPid -ErrorAction SilentlyContinue)) 'Timed-out inventory child process must be gone before the guard continues'
            $payloadJson = '{"Packages":[],"Devices":[]}'
            $payload = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($payloadJson))
            $markerLine = "DRIVER_INSTALL_SAFETY_INVENTORY:$payload"
            $markerOutput = Invoke-DriverInstallSafetyQuery -ScriptText "Write-Output '$markerLine'" -TimeoutSeconds 5
            $decoded = ConvertFrom-DriverInstallSafetyInventoryResult -OutputText $markerOutput
            Assert-That ($decoded.Packages.Count -eq 0 -and $decoded.Devices.Count -eq 0) 'Valid empty inventories must remain distinguishable from query failures'
            $malformedBlocked = $false
            try { $null = ConvertFrom-DriverInstallSafetyInventoryResult -OutputText 'not-an-inventory' } catch { $malformedBlocked = $true }
            Assert-That $malformedBlocked 'A missing inventory result must fail closed'
        }
        Set-Target '1.2.0.10'
        Assert-That (Invoke-Decision) "${driver}: trusted missing must install"
        Set-Installed '1.2.0.9'
        Assert-That (Invoke-Decision) "${driver}: numeric older must upgrade even with newer INF date"
        Set-Installed '1.2.0.10'
        Assert-That (!(Invoke-Decision)) "${driver}: equal must skip"
        Set-Installed '1.2.0.11'
        Assert-That (!(Invoke-Decision)) "${driver}: newer must skip"
        $script:fakePackages=@()
        Assert-That (!(Invoke-Decision)) "${driver}: active-only newer must skip"
        Set-Installed '1.2.0.11'; $script:fakeDevices=@()
        Assert-That (!(Invoke-Decision)) "${driver}: store-only newer must skip"
        $script:fakePackages=@(); $script:fakeDevices=@()
        $localBlocked=$false
        try { $null=Test-SafeDriverInstall -InfPath $targetInf -CatalogPath 'mock.cat' -HardwareId 'ROOT\TEST' -LocalTestPackage $true } catch { $localBlocked=$true }
        Assert-That ($localBlocked -eq ($driver -ne 'vhf-gamepad')) 'Only authorized VHF local-test packages may use existing Windows trust'
        foreach ($mode in @('unsigned','untrusted','self-signed')) {
            $script:fakeSignature = switch ($mode) {
                unsigned { [pscustomobject]@{Status='NotSigned';SignerCertificate=$null} }
                untrusted { [pscustomobject]@{Status='NotTrusted';SignerCertificate=[pscustomobject]@{Subject='CN=X';Issuer='CN=CA'}} }
                self-signed { [pscustomobject]@{Status='Valid';SignerCertificate=[pscustomobject]@{Subject='CN=X';Issuer='CN=X'}} }
            }
            $script:fakePackages=@(); $script:fakeDevices=@()
            if ($mode -eq 'self-signed') {
                Assert-That (Invoke-Decision) 'Verified expected selfsigned missing must be eligible'
                Set-Installed '1.2.0.9'; Assert-That (Invoke-Decision) 'Verified expected selfsigned newer must be eligible'
            } else {
                Expect-Blocked "${driver}: $mode missing must block"
                Set-Installed '1.2.0.9'; Expect-Blocked "${driver}: $mode newer candidate must block"
            }
            Set-Installed '1.2.0.10'; Assert-That (!(Invoke-Decision)) "${driver}: $mode equal must preserve installed"
            Set-Installed '1.2.0.11'; Assert-That (!(Invoke-Decision)) "${driver}: $mode older candidate must preserve installed"
        }
        $script:signatureChecks=0
        $script:queryFails=$true; Expect-Blocked 'Driver Store query failure must block'; Assert-That ($script:signatureChecks -eq 0) 'Inventory timeout must stop before package validation'; $script:queryFails=$false
        $script:cimFails=$true; Expect-Blocked 'CIM query failure must block'; Assert-That ($script:signatureChecks -eq 0) 'Inventory provider failure must stop before package validation'; $script:cimFails=$false
        $script:fakeDevices=@([pscustomobject]@{DeviceID='ROOT\TEST\0000';DriverVersion='unknown'})
        Expect-Blocked 'Unknown active version must block'
        $script:fakeDevices=@(); Set-Target 'unknown'; Expect-Blocked 'Unknown target version must block'
        Set-Target '1.2.0.10'; 'unreadable version' | Set-Content $storeInf; Expect-Blocked 'Unknown store INF must block'
        Assert-That ($text -notmatch '(?m)^\s+Ensure-LocalTestCertificateTrusted -') 'Automatic local certificate trust must not run'
        Assert-That ($text -notmatch "(?m)^Install-Certificate(?:IfPresent)? -StoreName") 'Automatic bundled certificate trust must not run'
        if ($driver -eq 'vhf-gamepad') {
            Assert-That (!$text.Contains('Import-Certificate')) 'VHF installer must never import a local test certificate'
            Assert-That (!$text.Contains('Refusing a local-test package without -AllowLocalTestCertificate')) 'Prior Windows trust must not require automatic trust opt-in'
            $install = $ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Install-DriverPackage'}, $true).Extent.Text
            Assert-That ($install.IndexOf('Test-SafeDriverInstall') -lt $install.IndexOf('Assert-DriverPackage')) 'VHF guard must precede trust/package assertion'
            Assert-That ($install.Contains("-HardwareId 'ROOT\VIBESHINEVIRTUALGAMEPAD'")) 'VHF guard hardware identity must match INF'
            Assert-That ($text.Contains('[int]$DailyUnsignedPolicy = 0')) 'Stable VHF publisher handling must remain the default'
            Assert-That ($install.Contains('if ($DailyUnsignedPolicy -eq 0 -and $null -ne $package.ProductionPublisherCertificate)')) 'Daily VHF must not import a production publisher'
        } else {
            $body=$ast.EndBlock.Extent.Text
            $guard=$body.LastIndexOf('Test-SafeDriverInstall -InfPath')
            $assertion=if ($driver -eq 'sunshine') {$body.LastIndexOf("`nAssert-Package")} else {$body.LastIndexOf("`nAssert-RequiredInstallArtifacts")}
            Assert-That ($guard -ge 0 -and $guard -lt $assertion) 'Display guard must precede package assertion'
            if ($driver -eq 'sudovda') {
                $start = $body.IndexOf('# Let Windows validate INF/DLL catalog membership')
                $end = $body.IndexOf("Write-Host '[SudoVDA] Creating virtual display device.'", $start)
                Assert-That ($start -ge 0 -and $end -gt $start) 'SudoVDA must stage before node removal'
                $segment = $body.Substring($start, $end - $start)
                $script:removed = $false
                function Invoke-DriverStep {
                    param($FilePath, $ArgumentList, $Description)
                    if ($ArgumentList[0] -eq '/add-driver') { throw 'Mock invalid catalog membership' }
                    if ($ArgumentList[0] -eq '--remove-device-node') { $script:removed = $true }
                }
                $failed = $false
                try { . ([scriptblock]::Create($segment)) } catch { $failed = $true }
                Assert-That ($failed -and !$script:removed) 'Failed SudoVDA staging must preserve existing node'
            } else {
                $stage = $body.LastIndexOf("-ArgumentList @('/add-driver', " + '$infPath' + ')')
                $stop = $body.LastIndexOf("`nInstall-SunshineVirtualDisplayDriver")
                Assert-That ($stage -gt $guard -and $stage -lt $stop) 'Display catalog staging must precede driver setup and its service stops'
                Assert-That ($text.Contains('[int]$DailyUnsignedPolicy = 0')) 'Stable display publisher handling must remain the default'
                Assert-That ($body.Contains('Install-ExpectedDisplayTrust')) 'Display trust must use independently verified release identity'
            }
        }
        Write-Host "$driver safety matrix passed (mocked queries/signatures only)."
    }
    $wix=Get-Content (Join-Path $RepositoryRoot 'packaging/windows/wix/patch_custom_actions.wxs') -Raw
    foreach ($action in @('UninstallSudovda','UninstallVirtualDisplayDriver','RemoveVirtualGamepadRoot')) {
        Assert-That ($wix -match ('<Custom Action="'+$action+'"[^>]*>[^<]*NOT UPGRADINGPRODUCTCODE')) "Upgrade driver removal must be guarded: $action"
    }
    $bootstrap=Get-Content (Join-Path $RepositoryRoot 'packaging/windows/bootstrapper/VibeshineInstaller.cs') -Raw
    Assert-That ($bootstrap.Contains('args.Add("UPGRADINGPRODUCTCODE=1")')) 'Bootstrapper pre-uninstall must preserve drivers'
    Assert-That (([regex]::Matches($bootstrap, 'args.Add\("UPGRADINGPRODUCTCODE=1"\)')).Count -eq 2) 'Both MSI pre-uninstall routes must preserve drivers'
    Assert-That ($bootstrap.Contains('logPhase.StartsWith("cli_remove_"')) 'CLI pre-uninstall must preserve drivers'
    # Execute the production stash/pre-uninstall/recovery methods with external
    # boundaries stubbed. A removed warning string cannot establish ordering or
    # prove that transactional replacements avoid standalone pre-uninstall.
    & (Join-Path $RepositoryRoot 'packaging/windows/bootstrapper/tests/test_bootstrap_rollback.ps1') -OutputDirectory (Join-Path $testRoot 'rollback')
    & (Join-Path $RepositoryRoot 'scripts/tests/test_display_producer_provenance.ps1') -RepositoryRoot $RepositoryRoot
    & (Join-Path $RepositoryRoot 'packaging/windows/test_gamepad_certificate_trust.ps1') -RepositoryRoot $RepositoryRoot
    & (Join-Path $RepositoryRoot 'packaging/windows/bootstrapper/tests/test_driver_selections.ps1')
    & (Join-Path $RepositoryRoot 'packaging/windows/bootstrapper/tests/test_gamepad_certificate_consent.ps1')
    $windowsPowerShell = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    & $windowsPowerShell -NoProfile -File (Join-Path $RepositoryRoot 'packaging/windows/test_gamepad_certificate_crypto.ps1') -RepositoryRoot $RepositoryRoot
    if ($LASTEXITCODE -ne 0) { throw 'Read-only native VHF certificate verification tests failed.' }
    & $windowsPowerShell -NoProfile -File (Join-Path $RepositoryRoot 'packaging/windows/test_display_release_trust.ps1') -RepositoryRoot $RepositoryRoot
    if ($LASTEXITCODE -ne 0) { throw 'Display release identity/trust validation tests failed.' }
    & (Join-Path $RepositoryRoot 'packaging/windows/test_gamepad_cleanup_paths.ps1') -RepositoryRoot $RepositoryRoot
    & (Join-Path $RepositoryRoot 'packaging/windows/test_optional_display_guard.ps1') -RepositoryRoot $RepositoryRoot
    & (Join-Path $RepositoryRoot 'packaging/windows/test_retained_display_device.ps1') -RepositoryRoot $RepositoryRoot
    & (Join-Path $RepositoryRoot 'packaging/windows/test_display_reboot_state.ps1') -RepositoryRoot $RepositoryRoot
    Write-Host 'Driver installation safety checks passed. No installers, drivers, trust stores, services or restarts were executed.'
} finally {
    # Only delete the exact uniquely created scratch directory.
    $resolvedTestRoot = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $testRoot -ErrorAction Stop).Path)
    $expectedTestRoot = [IO.Path]::GetFullPath($testRoot)
    $resolvedTempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedTestRoot.Equals($expectedTestRoot, [StringComparison]::OrdinalIgnoreCase) -or
        -not $resolvedTestRoot.StartsWith($resolvedTempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolvedTestRoot) -notmatch '^driver-safety-[0-9a-f]{32}$') {
        throw 'Refusing scratch cleanup outside the exact driver-safety temporary directory.'
    }
    Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
}
