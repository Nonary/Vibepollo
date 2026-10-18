param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path,
    [string]$DriverScript,
    [string]$FixtureCase
)
$ErrorActionPreference = 'Stop'
if (!$DriverScript) { $DriverScript = Join-Path $RepositoryRoot 'src_assets/windows/drivers/sunshine/install.ps1' }
$cases = @(
    'healthy-equal', 'healthy-newer', 'healthy-active-only', 'missing-equal', 'missing-newer',
    'phantom-newer', 'broken-10', 'broken-28', 'broken-31', 'disabled', 'reboot-pending',
    'sticky-reboot', 'unknown-health', 'multiple-devices', 'ambiguous-active', 'inventory-failure', 'health-query-failure',
    'unknown-active-version', 'unknown-present-version', 'broken-without-version', 'unknown-store-version', 'unsafe-store-path', 'published-mismatch',
    'untrusted-store', 'invalid-store-payload', 'no-newer-store', 'repair-fails', 'repair-still-missing',
    'repair-still-broken', 'repair-reports-older', 'native-reboot', 'fresh-install', 'older-upgrade',
    'validate-only', 'uninstall', 'optional-repair-failure'
)
if (!$FixtureCase) {
    $hostPath = (Get-Process -Id $PID).Path
    foreach ($case in $cases) {
        & $hostPath -NoLogo -NoProfile -NonInteractive -File $PSCommandPath -RepositoryRoot $RepositoryRoot -DriverScript $DriverScript -FixtureCase $case
        if ($LASTEXITCODE -ne 0) { throw "Retained display device fixture failed: $case" }
    }
    # Installation opt-outs must bypass the driver script entirely.
    [xml]$wix = Get-Content (Join-Path $RepositoryRoot 'packaging/windows/wix/patch_custom_actions.wxs') -Raw
    # CPackWiXPatch is an unnamespaced patch document, unlike custom_actions.wxs.
    $action = $wix.SelectSingleNode('//Custom[@Action="InstallVirtualDisplayDriver"]')
    if (!$action -or !$action.InnerText.Contains('NOT REMOVE') -or
        !$action.InnerText.Contains('INSTALL_DISPLAY_DRIVERS = "1"') -or
        !$action.InnerText.Contains('INSTALL_VIRTUAL_DISPLAY_DRIVER <> "0"')) {
        throw 'Explicit MSI display-driver opt-outs must remain authoritative.'
    }
    Write-Host "$($cases.Count) production guard/device repair fixtures and MSI opt-out wiring passed. Windows inventory, signature verification and process boundaries were mocked; no driver, trust, service or display operation ran."
    exit 0
}
if ($FixtureCase -notin $cases) { throw "Unknown fixture: $FixtureCase" }
function Assert-That([bool]$Condition, [string]$Message) { if (!$Condition) { throw "${FixtureCase}: $Message" } }
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('retained-display-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($DriverScript, [ref]$tokens, [ref]$errors)
    Assert-That ($errors.Count -eq 0) 'Installer must parse'
    foreach ($name in @('ConvertTo-DriverComponentVersion','Get-InfComponentVersion','Test-SafeDriverInstall','Get-SunshineDeviceHealth','Get-RetainedSunshinePackage','Repair-RetainedSunshineDevice')) {
        $function = $ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name}.GetNewClosure(), $true)
        if ($function) { . ([scriptblock]::Create($function.Extent.Text)) }
    }
    $guard = @($ast.EndBlock.Statements | Where-Object { $_.Extent.Text.Contains('Test-SafeDriverInstall -InfPath') })
    Assert-That ($guard.Count -eq 1) 'Exactly one production installation guard is required'
    $hardwareId = 'Root\SunshineVirtualDisplay'; $classGuid = '{4D36E968-E325-11CE-BFC1-08002BE10318}'
    $scriptDir = $scratch; $nefConc = Join-Path $scratch 'nefconc.exe'; $certPath = Join-Path $scratch 'public.cer'
    $env:windir = Join-Path $scratch 'windows'
    $target = [version]'1.6.4.0'; $newer = [version]'1.6.5.0'
    $infPath = Join-Path $scratch 'SunshineVirtualDisplayDriver.inf'; $catPath = Join-Path $scratch 'bundled.cat'
    "[Version]`nDriverVer=01/01/2000,$target" | Set-Content -LiteralPath $infPath
    $script:packages = @(); $script:nodes = @(); $script:signed = @(); $script:calls = @(); $script:verified = @()
    $script:rebootRequired = $FixtureCase -eq 'sticky-reboot'; $script:continued = $false
    $Uninstall = $FixtureCase -eq 'uninstall'; $ValidateOnly = $FixtureCase -eq 'validate-only'
    $InstallerBestEffort = $FixtureCase -eq 'optional-repair-failure'
    $version = if ($FixtureCase -in @('healthy-equal','missing-equal','fresh-install','older-upgrade')) { $target } else { $newer }
    function Add-Package([string]$Version, [string]$Name) {
        $dir = Join-Path $env:windir "System32/DriverStore/FileRepository/$Name"
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
        $path = Join-Path $dir 'SunshineVirtualDisplayDriver.inf'
        "[Version]`nDriverVer=01/01/2000,$Version" | Set-Content -LiteralPath $path
        $published = 'oem' + ($script:packages.Count + 1) + '.inf'
        New-Item -ItemType Directory -Path (Join-Path $env:windir 'INF') -Force | Out-Null
        Copy-Item -LiteralPath $path -Destination (Join-Path (Join-Path $env:windir 'INF') $published)
        $script:packages += [pscustomobject]@{ OriginalFileName=$path; Driver=$published }
        return $path
    }
    $selectedInf = Add-Package ([string]$version) 'best'
    # Unordered inventories must not choose a lower version or use INF dates.
    $null = Add-Package '1.6.3.99' 'old'
    $script:packages = @($script:packages[1],$script:packages[0])
    $node = [pscustomobject]@{ HardwareID=@($hardwareId); DeviceID='ROOT\DISPLAY\0000'; Present=$true; ConfigManagerErrorCode=0 }
    $driver = [pscustomobject]@{ HardwareID=$hardwareId; DeviceID=$node.DeviceID; DriverVersion=[string]$version }
    if ($FixtureCase -notin @('missing-equal','missing-newer','fresh-install','native-reboot','sticky-reboot','repair-fails','repair-still-missing','repair-still-broken','repair-reports-older','untrusted-store','invalid-store-payload','unsafe-store-path','published-mismatch','unknown-store-version','inventory-failure','health-query-failure','optional-repair-failure')) {
        $script:nodes=@($node); $script:signed=@($driver)
    }
    switch ($FixtureCase) {
        'healthy-active-only' { $script:packages=@() }
        'phantom-newer' { $node.Present=$false }
        'broken-10' { $node.ConfigManagerErrorCode=10 }
        'broken-28' { $node.ConfigManagerErrorCode=28 }
        'broken-31' { $node.ConfigManagerErrorCode=31 }
        'disabled' { $node.ConfigManagerErrorCode=22 }
        'reboot-pending' { $node.ConfigManagerErrorCode=14 }
        'unknown-health' { $node.Present=$null }
        'multiple-devices' { $script:nodes=@($node,$node) }
        'ambiguous-active' { $script:signed=@($driver,$driver) }
        'unknown-active-version' { $driver.DriverVersion='unknown' }
        'unknown-present-version' { $script:signed=@() }
        'broken-without-version' { $node.ConfigManagerErrorCode=31; $script:signed=@() }
        'unknown-store-version' { 'unknown' | Set-Content -LiteralPath $selectedInf }
        'unsafe-store-path' { $outside=Join-Path $scratch 'outside/SunshineVirtualDisplayDriver.inf'; New-Item -ItemType Directory -Path (Split-Path $outside) | Out-Null; Copy-Item $selectedInf $outside; $script:packages[1].OriginalFileName=$outside }
        'published-mismatch' { "[Version]`nDriverVer=01/01/2000,9.9.9.9" | Set-Content -LiteralPath (Join-Path $env:windir 'INF/oem1.inf') }
        'no-newer-store' { $node.ConfigManagerErrorCode=31; $script:packages=@($script:packages[0]) }
        'fresh-install' { $script:packages=@(); $script:nodes=@(); $script:signed=@() }
        'older-upgrade' { $script:packages=@($script:packages[0]); $driver.DriverVersion='1.6.3.99' }
    }
    function Get-WindowsDriver { param([switch]$Online,[switch]$All,$ErrorAction) if ($FixtureCase -eq 'inventory-failure') { throw 'Fixture inventory failure' }; $script:packages }
    function Get-CimInstance {
        param($ClassName,$ErrorAction)
        if ($FixtureCase -eq 'health-query-failure' -and $ClassName -eq 'Win32_PnPEntity') { throw 'Fixture health query failure' }
        if ($ClassName -eq 'Win32_PnPEntity') { return $script:nodes }
        if ($ClassName -eq 'Win32_PnPSignedDriver') { return $script:signed }
        throw "Unexpected Windows query: $ClassName"
    }
    function Assert-ExpectedDisplayPackage {
        param($InfPath,$CatalogPath,$HardwareId,$CertificatePath,$DriverPath)
        $script:verified += $InfPath
        if ($FixtureCase -eq 'invalid-store-payload') { throw 'Fixture catalog membership failure' }
        [pscustomobject]@{Certificate=[pscustomobject]@{Thumbprint='verified-signer'}}
    }
    function Get-AuthenticodeSignature { param($LiteralPath,$ErrorAction) [pscustomobject]@{Status=if ($FixtureCase -eq 'untrusted-store') {'NotTrusted'} else {'Valid'}; SignerCertificate=[pscustomobject]@{Thumbprint='verified-signer'}} }
    function Assert-Administrator {}
    function Assert-Artifact { param($Path) }
    function Resolve-SystemToolPath { param($ToolName) $ToolName }
    function Initialize-DriverStateRegistryAccess { $script:calls += 'registry' }
    function Stop-ApolloForDriverInstall { $script:calls += 'stop-service' }
    function Stop-VirtualDisplayBrokerForDriverInstall { $script:calls += 'stop-broker' }
    function Start-VirtualDisplayBrokerIfNeeded { $script:calls += 'restore-broker' }
    function Start-ApolloServiceIfNeeded { $script:calls += 'restore-service' }
    function Write-DriverRestartRequired { $script:calls += 'reboot-warning' }
    function Invoke-DriverProcess {
        param($FilePath,$ArgumentList,$AllowedExitCodes)
        $script:calls += ($ArgumentList -join '|')
        if ($FixtureCase -in @('repair-fails','optional-repair-failure')) { throw 'Fixture process failure' }
        if ($FixtureCase -eq 'native-reboot') { $script:rebootRequired=$true; return }
        if ($ArgumentList[0] -eq '/scan-devices' -and $FixtureCase -ne 'repair-still-missing') {
            $node.Present=$true; $node.ConfigManagerErrorCode=if ($FixtureCase -eq 'repair-still-broken') {31} else {0}
            $driver.DriverVersion=if ($FixtureCase -eq 'repair-reports-older') {'1.6.3.99'} else {[string]$version}
            $script:nodes=@($node); $script:signed=@($driver)
        }
    }
    $failure = $null
    try {
        . ([scriptblock]::Create($guard[0].Extent.Text))
        $script:continued=$true
    } catch { $failure=$_ } finally {
        $repairCases=@('missing-equal','missing-newer','phantom-newer','broken-10','broken-28','broken-31')
        $continueCases=@('fresh-install','older-upgrade','validate-only','uninstall')
        $noErrorCases=@('healthy-equal','healthy-newer','healthy-active-only','reboot-pending','sticky-reboot','native-reboot','optional-repair-failure') + $repairCases + $continueCases
        Assert-That (($null -eq $failure) -eq ($FixtureCase -in $noErrorCases)) "Unexpected guard result: $failure"
        Assert-That ($script:continued -eq ($FixtureCase -in $continueCases)) 'Guard continued into bundled installation unexpectedly'
        Assert-That (-not ($script:calls -match 'delete-driver|remove-device|disable-device|enable-device')) 'Retained repair must not delete packages/nodes or change disabled policy'
        Assert-That (-not ($script:calls -match [regex]::Escape($infPath))) 'Retained repair must not install the bundled older INF'
        if ($FixtureCase -in $repairCases) {
            Assert-That ($script:calls -contains "/add-driver|$selectedInf|/install") 'Must bind the highest equal/newer retained INF'
            Assert-That ($script:verified -contains $selectedInf) 'Selected retained payload must be independently verified'
            Assert-That ($script:calls -contains 'registry') 'Successful recovery must complete device-state initialization'
            $creates=@($script:calls | Where-Object { $_.StartsWith('--create-device-node|') })
            Assert-That ($creates.Count -eq $(if ($FixtureCase.StartsWith('broken-')) {0} else {1})) 'Node creation must follow actual presence'
            Assert-That ((@($script:calls -match '^/restart-device\|').Count -gt 0) -eq $FixtureCase.StartsWith('broken-')) 'Only present failed devices may restart'
        } elseif ($FixtureCase -in @('reboot-pending','sticky-reboot','native-reboot')) {
            Assert-That ($script:calls -contains 'reboot-warning') 'Reboot must stop further repair and emit the established marker'
            Assert-That (-not ($script:calls -match '^/add-driver|^/restart-device|^/scan-devices')) 'No native work may follow a reboot-required result'
            Assert-That (-not ($script:calls -contains 'registry')) 'Reboot must defer device-state mutation'
        } elseif ($FixtureCase -notin @('repair-fails','repair-still-missing','repair-still-broken','repair-reports-older','optional-repair-failure')) {
            Assert-That ($script:calls.Count -eq 0) 'Healthy, rejected or bypassed paths must not mutate devices'
        }
        if ($script:calls -contains 'stop-service') {
            Assert-That ($script:calls[0] -eq 'stop-service' -and $script:calls[1] -eq 'stop-broker') 'Runtime must stop before device operations'
            Assert-That ($script:calls[-2] -eq 'restore-broker' -and $script:calls[-1] -eq 'restore-service') 'Runtime restoration must run after success, failure or reboot'
        }
        Write-Host "PASS $FixtureCase"
    }
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force
}
