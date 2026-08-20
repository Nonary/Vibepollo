# Pulls the real function out of install.ps1 and exercises it, so the test
# covers the shipped logic rather than a restatement of it.
$ErrorActionPreference = 'Stop'
$installScript = 'D:\sources\worktrees\vhf_gamepad_large\src_assets\windows\drivers\sunshine\install.ps1'

$ast = [System.Management.Automation.Language.Parser]::ParseFile($installScript, [ref]$null, [ref]$null)
$wanted = @('Test-CatalogTrustedIndependently')
$definitions = $ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $wanted -contains $node.Name
}, $true)
if ($definitions.Count -ne $wanted.Count) {
    throw "Expected $($wanted.Count) function(s), extracted $($definitions.Count)."
}

# The subject constant the functions close over.
$legacySelfSignedSubject = 'CN=Sunshine Virtual Display Release Signing'
foreach ($definition in $definitions) {
    . ([scriptblock]::Create($definition.Extent.Text))
}

$failures = 0
function Check {
    param([bool] $Actual, [bool] $Expected, [string] $What)
    $ok = $Actual -eq $Expected
    "{0,-62} {1}" -f $What, $(if ($ok) { 'ok' } else { "FAILED (got $Actual)" })
    if (-not $ok) { $script:failures++ }
}

$pkg = 'C:\Users\Chase\.claude\jobs\04a34b03\tmp\msi-layout\Sunshine\drivers\sunshine'

# 1. Today's package: self-signed catalog, .cer alongside it. The trust must be
#    kept, or upgrading to this build would break driver installation.
$catPath = Join-Path $pkg 'SunshineVirtualDisplayDriver.cat'
$certPath = Join-Path $pkg 'SunshineVirtualDisplayDriver.cer'
Check (Test-CatalogTrustedIndependently) $false 'self-signed catalog still needs its root'

# 2. Same catalog, but with no .cer shipped. Still self-signed, so still
#    dependent: the subject check has to catch it without the file.
$certPath = Join-Path $pkg 'does-not-exist.cer'
Check (Test-CatalogTrustedIndependently) $false 'self-signed catalog detected without the .cer'

# 3. A catalog signed by a real CA. Windows ships plenty; any of them stands on
#    its own, which is the state a SignPath-signed catalog will be in.
$microsoftCatalog = Get-ChildItem 'C:\Windows\System32\CatRoot' -Recurse -Filter *.cat -ErrorAction SilentlyContinue |
    Where-Object { (Get-AuthenticodeSignature -LiteralPath $_.FullName).Status -eq 'Valid' } |
    Select-Object -First 1
if ($null -eq $microsoftCatalog) {
    'no CA-signed catalog available to test against                 SKIPPED'
} else {
    $catPath = $microsoftCatalog.FullName
    $certPath = Join-Path $pkg 'SunshineVirtualDisplayDriver.cer'
    Check (Test-CatalogTrustedIndependently) $true "CA-signed catalog stands alone ($($microsoftCatalog.Name))"
}

# 4. An unsigned file must never be read as independently trusted.
$catPath = Join-Path $pkg 'virtualdisplay_probe.exe'
Check (Test-CatalogTrustedIndependently) $false 'unsigned file is not independently trusted'

""
if ($failures -eq 0) { 'ALL CHECKS PASSED' } else { "$failures CHECK(S) FAILED" }
exit $failures
