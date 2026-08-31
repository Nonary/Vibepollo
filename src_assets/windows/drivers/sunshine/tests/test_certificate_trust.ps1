# Pulls the real signer-validation helper out of install.ps1 and exercises it,
# so the test covers the shipped trust boundary rather than restating it.
$ErrorActionPreference = 'Stop'
$pkg = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$installScript = Join-Path $pkg 'install.ps1'

$ast = [System.Management.Automation.Language.Parser]::ParseFile($installScript, [ref]$null, [ref]$null)
$wanted = @('Get-ExpectedCatalogSignerCertificate')
$definitions = $ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $wanted -contains $node.Name
}, $true)
if ($definitions.Count -ne $wanted.Count) {
    throw "Expected $($wanted.Count) function(s), extracted $($definitions.Count)."
}

$signPathSignerSubject = 'CN=SignPath Foundation, O=SignPath Foundation, L=Lewes, S=Delaware, C=US'
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

$script:testSignature = $null
function Get-AuthenticodeSignature {
    param([string] $LiteralPath)
    return $script:testSignature
}

$catPath = Join-Path $pkg 'SunshineVirtualDisplayDriver.cat'

$script:testSignature = [pscustomobject]@{
    Status = 'NotSigned'
    SignerCertificate = $null
}
Check ($null -eq (Get-ExpectedCatalogSignerCertificate)) $true 'unsigned catalog is not trusted as a publisher'

$unexpectedSigner = [pscustomobject]@{
    Subject = 'CN=Unexpected Driver Publisher, O=Example'
    Thumbprint = '00112233445566778899AABBCCDDEEFF00112233'
}
$script:testSignature = [pscustomobject]@{
    Status = 'Valid'
    SignerCertificate = $unexpectedSigner
}
Check ($null -eq (Get-ExpectedCatalogSignerCertificate)) $true 'valid catalog from an unexpected publisher is rejected'

$expectedSigner = [pscustomobject]@{
    Subject = $signPathSignerSubject
    Thumbprint = '112233445566778899AABBCCDDEEFF0011223344'
}
$script:testSignature = [pscustomobject]@{
    Status = 'Valid'
    SignerCertificate = $expectedSigner
}
$acceptedSigner = Get-ExpectedCatalogSignerCertificate
Check ($null -ne $acceptedSigner) $true 'valid SignPath publisher is accepted'
Check ([string]::Equals($acceptedSigner.Thumbprint, $expectedSigner.Thumbprint, [System.StringComparison]::OrdinalIgnoreCase)) $true 'accepted publisher preserves its exact thumbprint'

$script:testSignature.SignerCertificate.Subject = $signPathSignerSubject.ToLowerInvariant()
Check ($null -ne (Get-ExpectedCatalogSignerCertificate)) $true 'SignPath subject comparison is case-insensitive'

""
if ($failures -eq 0) { 'ALL CHECKS PASSED' } else { "$failures CHECK(S) FAILED" }
exit $failures
