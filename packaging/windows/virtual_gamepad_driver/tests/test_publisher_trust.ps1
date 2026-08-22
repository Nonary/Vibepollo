$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$installScript = Join-Path $repositoryRoot 'src_assets/windows/drivers/vhf-gamepad/install.ps1'
$workflowPath = Join-Path $repositoryRoot '.github/workflows/ci-windows.yml'
$windowsPackagingCmake = Join-Path $repositoryRoot 'cmake/packaging/windows.cmake'
$expectedSignPathFoundationSignerSubject = 'CN=SignPath Foundation, O=SignPath Foundation, L=Lewes, S=Delaware, C=US'
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $installScript,
    [ref] $tokens,
    [ref] $parseErrors)
if ($parseErrors.Count -ne 0) {
    throw "install.ps1 has parse errors: $($parseErrors.Message -join '; ')"
}

$installFileText = Get-Content -LiteralPath $installScript -Raw
$expectedSubjectAssignment = "`$expectedSignPathFoundationSignerSubject = '$expectedSignPathFoundationSignerSubject'"
if ($installFileText.IndexOf($expectedSubjectAssignment, [System.StringComparison]::Ordinal) -lt 0) {
    throw 'install.ps1 does not pin the expected SignPath Foundation signer subject.'
}

$wanted = @(
    'Get-Thumbprint',
    'Get-ValidatedSharedPublisherCertificate',
    'Ensure-ProductionPublisherTrusted'
)
$definitions = @($ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $wanted -contains $node.Name
}, $true))
if ($definitions.Count -ne $wanted.Count) {
    throw "Expected $($wanted.Count) testable trust functions, found $($definitions.Count)."
}
foreach ($definition in $definitions) {
    . ([scriptblock]::Create($definition.Extent.Text))
}

function New-TestSignature {
    param(
        [Parameter(Mandatory = $true)][string] $Thumbprint,
        [string] $Subject = $expectedSignPathFoundationSignerSubject,
        [System.Management.Automation.SignatureStatus] $Status = [System.Management.Automation.SignatureStatus]::Valid
    )
    return [PSCustomObject]@{
        Status = $Status
        SignerCertificate = [PSCustomObject]@{
            Thumbprint = $Thumbprint
            Subject = $Subject
        }
    }
}

$expected = New-TestSignature -Thumbprint '00112233445566778899AABBCCDDEEFF00112233'
$matching = New-TestSignature -Thumbprint '00 11 22 33 44 55 66 77 88 99 aa bb cc dd ee ff 00 11 22 33'
$selected = Get-ValidatedSharedPublisherCertificate `
    -CatalogSignature $expected `
    -ToolSignature $matching `
    -ExpectedSubject $expectedSignPathFoundationSignerSubject
if ((Get-Thumbprint -Certificate $selected) -ne '00112233445566778899AABBCCDDEEFF00112233') {
    throw 'Shared signer selection did not return the exact validated catalog signer.'
}

$mismatchRejected = $false
try {
    Get-ValidatedSharedPublisherCertificate `
        -CatalogSignature $expected `
        -ToolSignature (New-TestSignature -Thumbprint 'FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF') `
        -ExpectedSubject $expectedSignPathFoundationSignerSubject | Out-Null
} catch {
    $mismatchRejected = $true
}
if (-not $mismatchRejected) {
    throw 'Different catalog/setup signers were accepted.'
}

$unexpectedSubjectRejected = $false
try {
    Get-ValidatedSharedPublisherCertificate `
        -CatalogSignature $expected `
        -ToolSignature (New-TestSignature `
            -Thumbprint $expected.SignerCertificate.Thumbprint `
            -Subject 'CN=Unexpected Publisher, O=Unexpected Publisher, C=US') `
        -ExpectedSubject $expectedSignPathFoundationSignerSubject | Out-Null
} catch {
    $unexpectedSubjectRejected = $true
}
if (-not $unexpectedSubjectRejected) {
    throw 'An unexpected setup signer identity was accepted for publisher trust.'
}

$invalidRejected = $false
try {
    Get-ValidatedSharedPublisherCertificate `
        -CatalogSignature $expected `
        -ToolSignature (New-TestSignature -Thumbprint $expected.SignerCertificate.Thumbprint -Status HashMismatch) `
        -ExpectedSubject $expectedSignPathFoundationSignerSubject | Out-Null
} catch {
    $invalidRejected = $true
}
if (-not $invalidRejected) {
    throw 'A non-valid setup signature was accepted for publisher trust.'
}

# Exercise the store-mutating boundary with an unexpected identity. It must
# reject before constructing or opening LocalMachine\TrustedPublisher.
$unexpectedStoreSignerRejected = $false
try {
    Ensure-ProductionPublisherTrusted `
        -PublisherCertificate ([PSCustomObject]@{
            Thumbprint = $expected.SignerCertificate.Thumbprint
            Subject = 'CN=Unexpected Publisher, O=Unexpected Publisher, C=US'
        }) `
        -ExpectedSubject $expectedSignPathFoundationSignerSubject
} catch {
    $unexpectedStoreSignerRejected = $true
}
if (-not $unexpectedStoreSignerRejected) {
    throw 'The publisher store boundary accepted an unexpected signer identity.'
}

$trustFunction = @($definitions | Where-Object Name -eq 'Ensure-ProductionPublisherTrusted')
if ($trustFunction.Count -ne 1) {
    throw 'Ensure-ProductionPublisherTrusted was not found exactly once.'
}
$trustText = $trustFunction[0].Extent.Text
foreach ($requiredTrustOperation in @(
    "X509Store]::new('TrustedPublisher', 'LocalMachine')",
    '$store.Add($PublisherCertificate)',
    '[System.Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly',
    'Failed to establish publisher trust'
)) {
    if ($trustText.IndexOf($requiredTrustOperation, [System.StringComparison]::Ordinal) -lt 0) {
        throw "Publisher trust function lacks required operation: $requiredTrustOperation"
    }
}
$subjectGuardOffset = $trustText.IndexOf('[string]::Equals', [System.StringComparison]::Ordinal)
$storeConstructionOffset = $trustText.IndexOf("X509Store]::new('TrustedPublisher', 'LocalMachine')", [System.StringComparison]::Ordinal)
if ($subjectGuardOffset -lt 0 -or $storeConstructionOffset -lt 0 -or $subjectGuardOffset -ge $storeConstructionOffset) {
    throw 'Unexpected publisher identity is not rejected before the machine certificate store is accessed.'
}

$installFunction = @($ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Install-DriverPackage'
}, $true))
if ($installFunction.Count -ne 1) {
    throw 'Install-DriverPackage was not found exactly once.'
}
$installText = $installFunction[0].Extent.Text
$trustOffset = $installText.IndexOf('Ensure-ProductionPublisherTrusted', [System.StringComparison]::Ordinal)
$pnpOffset = $installText.IndexOf("Invoke-PnpUtil -Arguments @('/add-driver'", [System.StringComparison]::Ordinal)
if ($trustOffset -lt 0 -or $pnpOffset -lt 0 -or $trustOffset -ge $pnpOffset) {
    throw 'Production publisher trust is not established before PnPUtil driver staging.'
}

$expectedGate = @'
      - name: Test VHF publisher trust policy
        shell: pwsh
        run: .\packaging\windows\virtual_gamepad_driver\tests\test_publisher_trust.ps1
'@

function Test-WorkflowContainsPublisherTrustGate {
    param(
        [Parameter(Mandatory = $true)][string] $WorkflowText,
        [Parameter(Mandatory = $true)][string] $ExpectedGate
    )

    $normalizedWorkflowText = $WorkflowText.Replace("`r`n", "`n").Replace("`r", "`n")
    $normalizedExpectedGate = $ExpectedGate.Replace("`r`n", "`n").Replace("`r", "`n").TrimEnd()
    return $normalizedWorkflowText.IndexOf(
        $normalizedExpectedGate,
        [System.StringComparison]::Ordinal) -ge 0
}

$lfGateFixture = $expectedGate.Replace("`r`n", "`n").Replace("`r", "`n").TrimEnd()
$crlfGateFixture = $lfGateFixture.Replace("`n", "`r`n")
foreach ($validGateFixture in @($lfGateFixture, $crlfGateFixture)) {
    if (-not (Test-WorkflowContainsPublisherTrustGate `
            -WorkflowText $validGateFixture `
            -ExpectedGate $expectedGate)) {
        throw 'The publisher trust workflow gate rejected a valid newline style.'
    }
}
$missingGateFixture = $lfGateFixture.Replace(
    '.\packaging\windows\virtual_gamepad_driver\tests\test_publisher_trust.ps1',
    '.\packaging\windows\virtual_gamepad_driver\tests\missing.ps1')
if (Test-WorkflowContainsPublisherTrustGate `
        -WorkflowText $missingGateFixture `
        -ExpectedGate $expectedGate) {
    throw 'The publisher trust workflow gate accepted a missing policy test.'
}

$workflowText = Get-Content -LiteralPath $workflowPath -Raw
if (-not (Test-WorkflowContainsPublisherTrustGate `
        -WorkflowText $workflowText `
        -ExpectedGate $expectedGate)) {
    throw 'The ordinary/release Windows build does not run this publisher trust policy test.'
}

function Get-WorkflowLiteralRunBlock {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string[]] $Lines,
        [Parameter(Mandatory = $true)][string] $StepName
    )

    $stepPattern = '^(?<indent>\s*)- name: ' + [regex]::Escape($StepName) + '\s*$'
    for ($stepIndex = 0; $stepIndex -lt $Lines.Count; ++$stepIndex) {
        $stepMatch = [regex]::Match($Lines[$stepIndex], $stepPattern)
        if (-not $stepMatch.Success) {
            continue
        }
        $stepIndent = $stepMatch.Groups['indent'].Value.Length
        for ($runIndex = $stepIndex + 1; $runIndex -lt $Lines.Count; ++$runIndex) {
            $line = $Lines[$runIndex]
            if ($line -match '^\s*$') {
                continue
            }
            $lineIndent = ([regex]::Match($line, '^\s*')).Value.Length
            if ($lineIndent -le $stepIndent) {
                break
            }
            if ($line -notmatch '^\s*run:\s*\|\s*$') {
                continue
            }

            $scriptLines = New-Object System.Collections.Generic.List[string]
            $scriptIndent = $null
            for ($scriptIndex = $runIndex + 1; $scriptIndex -lt $Lines.Count; ++$scriptIndex) {
                $scriptLine = $Lines[$scriptIndex]
                if ($scriptLine -notmatch '^\s*$') {
                    $currentIndent = ([regex]::Match($scriptLine, '^\s*')).Value.Length
                    if ($currentIndent -le $stepIndent) {
                        break
                    }
                    if ($null -eq $scriptIndent) {
                        $scriptIndent = $currentIndent
                    }
                }
                if ($null -eq $scriptIndent) {
                    $scriptLines.Add('')
                } else {
                    $scriptLines.Add($scriptLine.Substring([Math]::Min($scriptIndent, $scriptLine.Length)))
                }
            }
            return $scriptLines -join "`n"
        }
    }
    throw "Workflow literal run block was not found: $StepName"
}

$workflowLines = Get-Content -LiteralPath $workflowPath
$postSignScript = Get-WorkflowLiteralRunBlock -Lines $workflowLines -StepName 'Verify SignPath signatures'
$postSignTokens = $null
$postSignParseErrors = $null
[System.Management.Automation.Language.Parser]::ParseInput(
    $postSignScript,
    [ref] $postSignTokens,
    [ref] $postSignParseErrors) | Out-Null
if ($postSignParseErrors.Count -ne 0) {
    throw "Verify SignPath signatures has parse errors: $($postSignParseErrors.Message -join '; ')"
}
foreach ($requiredPostSignCheck in @(
    "`$expectedSignPathFoundationSigner = '$expectedSignPathFoundationSignerSubject'",
    "`$catalogSignature.SignerCertificate.Subject -ne `$expectedSignPathFoundationSigner",
    "`$vhfCatalogSignature.SignerCertificate.Thumbprint -cne `$vhfToolSignature.SignerCertificate.Thumbprint",
    "`$vhfCatalogSignature.SignerCertificate.Subject -ne `$expectedSignPathFoundationSigner",
    "`$vhfToolSignature.SignerCertificate.Subject -ne `$expectedSignPathFoundationSigner"
)) {
    if ($postSignScript.IndexOf($requiredPostSignCheck, [System.StringComparison]::Ordinal) -lt 0) {
        throw "Post-sign VHF verification lacks required signer check: $requiredPostSignCheck"
    }
}

$packagingText = Get-Content -LiteralPath $windowsPackagingCmake -Raw
if ($packagingText.IndexOf('test_publisher_trust.ps1', [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
    throw 'The publisher trust policy test was added to the installed VHF payload.'
}
if (Test-Path -LiteralPath (Join-Path $repositoryRoot 'src_assets/windows/drivers/vhf-gamepad/tests/test_publisher_trust.ps1')) {
    throw 'The publisher trust policy test remains under the volatile installed payload root.'
}

Write-Host 'VHF publisher trust policy checks passed.'
