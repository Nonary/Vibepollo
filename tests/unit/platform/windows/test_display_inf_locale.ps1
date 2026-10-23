# Run in a fresh PowerShell process for each culture: -Culture tr-TR, en-US,
# az-Latn-AZ or invariant. PowerShell's -match regex cache retains its first culture.
param([string]$Culture = 'tr-TR', [string]$RepositoryRoot = '')

$ErrorActionPreference = 'Stop'
if (!$RepositoryRoot) { $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path }
$cultureInfo = if ($Culture -eq 'invariant') { [Globalization.CultureInfo]::InvariantCulture } else { [Globalization.CultureInfo]::GetCultureInfo($Culture) }
[Threading.Thread]::CurrentThread.CurrentCulture = $cultureInfo
[Threading.Thread]::CurrentThread.CurrentUICulture = $cultureInfo

function Import-InstallerFunction {
    param([string]$Path, [string]$Name)
    # Parse and import only the requested function. Never invoke installer entry points.
    $tokens = $null; $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw "Installer parse failed: $parseErrors" }
    $definitions = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $Name }, $false))
    if ($definitions.Count -ne 1) { throw "Expected one function $Name in $Path" }
    # Return the definition for dot-sourcing in the test scope.
    return [scriptblock]::Create($definitions[0].Extent.Text)
}

function Assert-IdentityResult {
    param([string]$Directory, [string]$InfName, [string]$Stem, [string]$HardwareId, [bool]$Valid, [string]$Case)
    $message = ''
    try {
        $null = Assert-ExpectedDisplayPackage -InfPath (Join-Path $Directory $InfName) -CatalogPath (Join-Path $Directory "$Stem.cat") -HardwareId $HardwareId -CertificatePath (Join-Path $Directory "$Stem.cer") -DriverPath (Join-Path $Directory "$Stem.dll")
    } catch { $message = $_.Exception.Message }
    $expected = if ($Valid) { 'IDENTITY_VALIDATION_PASSED' } else { 'Display INF class, catalog or hardware identity mismatch.' }
    if ($message -cne $expected) { throw "$Case under $Culture`: expected '$expected', received '$message'." }
    $script:checks++
}

# Stop at the next validation boundary: no certificate loading, trust changes,
# WinTrust calls, process execution, driver staging or installation are exercised.
function Initialize-DisplayPackageValidation { throw 'IDENTITY_VALIDATION_PASSED' }
$checks = 0
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('display-inf-locale-' + [Guid]::NewGuid().ToString('N'))
$oldSystemRoot = $env:SystemRoot
try {
    $null = New-Item -ItemType Directory -Path $temporaryRoot
    foreach ($installer in @('sudovda', 'sunshine')) {
        $sourceDirectory = Join-Path $RepositoryRoot "src_assets/windows/drivers/$installer"
        . (Import-InstallerFunction -Path (Join-Path $sourceDirectory 'install.ps1') -Name 'Assert-ExpectedDisplayPackage')
        foreach ($package in @(
            @{ Inf='SudoVDA.inf'; Stem='SudoVDA'; Hardware='root\sudomaker\sudovda'; Directory='sudovda' },
            @{ Inf='SunshineVirtualDisplayDriver.inf'; Stem='SunshineVirtualDisplayDriver'; Hardware='root\sunshinevirtualdisplay'; Directory='sunshine' }
        )) {
            $fixture = Join-Path $temporaryRoot ($installer + '-' + $package.Directory)
            $null = New-Item -ItemType Directory -Path $fixture
            $original = [IO.File]::ReadAllText((Join-Path $RepositoryRoot "src_assets/windows/drivers/$($package.Directory)/$($package.Inf)"))
            $infFile = Join-Path $fixture $package.Inf
            foreach ($variant in @('shipped', 'uppercase')) {
                $text = if ($variant -eq 'uppercase') { $original.ToUpperInvariant() } else { $original }
                [IO.File]::WriteAllText($infFile, $text, [Text.Encoding]::Unicode)
                Assert-IdentityResult $fixture $package.Inf $package.Stem $package.Hardware $true "$installer/$($package.Directory)/$variant"
            }
            # Keep comments/whitespace acceptance while checking each identity gate.
            $valid = "`r`n; comment before INF`r`n" + $original + "`r`n; trailing comment`r`n"
            [IO.File]::WriteAllText($infFile, $valid, [Text.Encoding]::Unicode)
            Assert-IdentityResult $fixture $package.Inf $package.Stem $package.Hardware $true "$installer/$($package.Directory)/comments"
            $badCases = @(
                @{ Name='class'; Text=[regex]::Replace($original, '(?im)^(\s*Class\s*=\s*)Display', '${1}System') },
                @{ Name='guid'; Text=$original.Replace('4D36E968','4D36E969').Replace('4d36e968','4d36e969') },
                @{ Name='catalog'; Text=[regex]::Replace($original, '(?im)^(\s*CatalogFile\s*=\s*)[^\r\n]+', '${1}other.cat') },
                @{ Name='hardware'; Text=[regex]::Replace($original, '(?im)^(\s*%[^%]+%\s*=\s*[^,\r\n]+,\s*)[^\r\n]+', '${1}Root\OtherDriver') }
            )
            foreach ($bad in $badCases) {
                if ($bad.Text -ceq $original) { throw "Negative fixture did not change: $($bad.Name)" }
                [IO.File]::WriteAllText($infFile, $bad.Text, [Text.Encoding]::Unicode)
                Assert-IdentityResult $fixture $package.Inf $package.Stem $package.Hardware $false "$installer/$($package.Directory)/$($bad.Name)"
            }
        }
    }

    $sunshineScript = Join-Path $RepositoryRoot 'src_assets/windows/drivers/sunshine/install.ps1'
    . (Import-InstallerFunction -Path $sunshineScript -Name 'Get-SunshineDriverPublishedNamesFromInfCache')
    $env:SystemRoot = Join-Path $temporaryRoot 'windows'
    $cache = Join-Path $env:SystemRoot 'INF'
    $null = New-Item -ItemType Directory -Path $cache
    $cacheInf = "[Version]`nClassGUID={4D36E968-E325-11CE-BFC1-08002BE10318}`nCatalogFile=SUNSHINEVIRTUALDISPLAYDRIVER.CAT`n[Models]`n%Device%=Install, ROOT\SUNSHINEVIRTUALDISPLAY`n"
    [IO.File]::WriteAllText((Join-Path $cache 'oem42.inf'), $cacheInf)
    [IO.File]::WriteAllText((Join-Path $cache 'oem43.inf'), $cacheInf.Replace('4D36E968', '4D36E969'))
    [IO.File]::WriteAllText((Join-Path $cache 'oem44.inf'), $cacheInf.Replace('SUNSHINEVIRTUALDISPLAYDRIVER.CAT', 'Other.cat'))
    [IO.File]::WriteAllText((Join-Path $cache 'oem45.inf'), $cacheInf.Replace('ROOT\SUNSHINEVIRTUALDISPLAY', 'ROOT\OTHERDRIVER'))
    $names = @(Get-SunshineDriverPublishedNamesFromInfCache)
    if ($names.Count -ne 1 -or $names[0] -cne 'oem42.inf') { throw "INF cache identity filtering under $Culture failed: $names" }
    $checks++
    Write-Output "PASS: $checks production INF identity checks under $Culture (PowerShell $($PSVersionTable.PSVersion))."
} finally {
    $env:SystemRoot = $oldSystemRoot
    if (Test-Path -LiteralPath $temporaryRoot) { Remove-Item -LiteralPath $temporaryRoot -Recurse -Force }
}
