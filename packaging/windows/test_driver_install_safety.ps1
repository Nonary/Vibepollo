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
            Assert-That ($block.Contains('$LocalTestPackage -and $AllowUntrustedLocalPackage -and $signature.Status -eq ''NotTrusted''')) 'Only a signed local VHF candidate may defer trust for independent validation and recipient consent'
        } elseif ($null -eq $baseline) { $baseline = $block } else { Assert-That ($block -ceq $baseline) "Safety policy drift: $driver" }
        # Load only these pure/read-only functions, never the installer body.
        foreach ($name in @('ConvertTo-DriverComponentVersion', 'Get-InfComponentVersion', 'Test-SafeDriverInstall')) {
            $function = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }.GetNewClosure(), $true)
            . ([scriptblock]::Create($function.Extent.Text))
        }
        $script:fakePackages = @(); $script:fakeDevices = @(); $script:queryFails = $false; $script:cimFails = $false
        $script:fakeSignature = [pscustomobject]@{ Status='Valid'; SignerCertificate=[pscustomobject]@{ Subject='CN=Publisher'; Issuer='CN=Public CA' } }
        function Get-WindowsDriver { param([switch]$Online,[switch]$All,$ErrorAction) if ($script:queryFails) { throw 'Mock query failure' }; return $script:fakePackages }
        function Get-CimInstance { param($ClassName,$ErrorAction) if ($script:cimFails) { throw 'Mock CIM failure' }; return $script:fakeDevices }
        function Get-AuthenticodeSignature { param($LiteralPath,$ErrorAction) return $script:fakeSignature }
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
            if ($driver -eq 'vhf-gamepad' -and $mode -eq 'self-signed') {
                Assert-That (Invoke-Decision) 'Already Windows-trusted selfsigned VHF missing must be eligible'
                Set-Installed '1.2.0.9'; Assert-That (Invoke-Decision) 'Already Windows-trusted selfsigned VHF newer must be eligible'
            } else {
                Expect-Blocked "${driver}: $mode missing must block"
                Set-Installed '1.2.0.9'; Expect-Blocked "${driver}: $mode newer candidate must block"
            }
            Set-Installed '1.2.0.10'; Assert-That (!(Invoke-Decision)) "${driver}: $mode equal must preserve installed"
            Set-Installed '1.2.0.11'; Assert-That (!(Invoke-Decision)) "${driver}: $mode older candidate must preserve installed"
        }
        $script:queryFails=$true; Expect-Blocked 'Driver Store query failure must block'; $script:queryFails=$false
        $script:cimFails=$true; Expect-Blocked 'CIM query failure must block'; $script:cimFails=$false
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
                $stop = [regex]::Match($body, '(?m)^Stop-(Sunshine|Apollo)ForDriverInstall').Index
                Assert-That ($stage -gt $guard -and $stage -lt $stop -and $stage -lt $body.LastIndexOf('Stop-VirtualDisplayBrokerForDriverInstall')) 'Display catalog staging must precede service stops'
                Assert-That ($text.Contains('[int]$DailyUnsignedPolicy = 0')) 'Stable display publisher handling must remain the default'
                Assert-That ($body.Contains('if ($DailyUnsignedPolicy -eq 0) { Install-CatalogSignerCertificate }')) 'Daily display must not import a production publisher'
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
    Assert-That ($bootstrap.Contains('its uninstaller cannot guarantee preservation of installed drivers')) 'Legacy pre-uninstall must fail safely'
    & (Join-Path $RepositoryRoot 'packaging/windows/test_gamepad_certificate_trust.ps1') -RepositoryRoot $RepositoryRoot
    & (Join-Path $RepositoryRoot 'packaging/windows/bootstrapper/tests/test_gamepad_certificate_consent.ps1')
    $windowsPowerShell = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    & $windowsPowerShell -NoProfile -File (Join-Path $RepositoryRoot 'packaging/windows/test_gamepad_certificate_crypto.ps1') -RepositoryRoot $RepositoryRoot
    if ($LASTEXITCODE -ne 0) { throw 'Read-only native VHF certificate verification tests failed.' }
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
