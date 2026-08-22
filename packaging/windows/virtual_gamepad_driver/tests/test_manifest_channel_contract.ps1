$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$refreshScript = Join-Path $repositoryRoot 'packaging/windows/virtual_gamepad_driver/refresh_driver_package.ps1'
$installScript = Join-Path $repositoryRoot 'src_assets/windows/drivers/vhf-gamepad/install.ps1'
$workflowPath = Join-Path $repositoryRoot '.github/workflows/ci-windows.yml'
$windowsPackagingCmake = Join-Path $repositoryRoot 'cmake/packaging/windows.cmake'
$manifestPayload = @(
    'driver/VibeshineVhfGamepad.inf',
    'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.cat',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)
$localTestCertificate = 'driver/VibeshineVhfGamepad.cer'
$msiRequestChannel = 'msi-request-signing'
$downstreamSignedFiles = @(
    'driver/VibeshineVhfGamepad.cat',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)
$contractFunctions = @(
    'Resolve-RequiredPath',
    'Assert-File',
    'Get-Sha256',
    'Assert-ExactList',
    'Get-ManifestEntry',
    'Assert-ManifestHash',
    'Assert-ManifestPayloadContract'
)

function Import-ManifestContract {
    param(
        [Parameter(Mandatory = $true)][string] $ScriptPath,
        [Parameter(Mandatory = $true)][string] $OwnerFunction
    )

    $tokens = $null
    $parseErrors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile(
        $ScriptPath,
        [ref] $tokens,
        [ref] $parseErrors)
    if ($parseErrors.Count -ne 0) {
        throw "$ScriptPath has parse errors: $($parseErrors.Message -join '; ')"
    }

    $definitions = @($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $contractFunctions -contains $node.Name
    }, $true))
    foreach ($required in @('Assert-File', 'Get-Sha256', 'Assert-ExactList', 'Assert-ManifestHash', 'Assert-ManifestPayloadContract')) {
        $matches = @($definitions | Where-Object Name -eq $required)
        if ($matches.Count -ne 1) {
            throw "$ScriptPath must define $required exactly once; found $($matches.Count)."
        }
    }

    $owner = @($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq $OwnerFunction
    }, $true))
    if ($owner.Count -ne 1 -or
        $owner[0].Extent.Text.IndexOf('Assert-ManifestPayloadContract', [System.StringComparison]::Ordinal) -lt 0) {
        throw "$OwnerFunction does not enforce the manifest payload contract."
    }

    foreach ($name in $contractFunctions) {
        $definition = @($definitions | Where-Object Name -eq $name)
        if ($definition.Count -eq 1) {
            $bodyText = $definition[0].Body.Extent.Text
            Set-Item -LiteralPath "function:script:$name" -Value ([scriptblock]::Create(
                $bodyText.Substring(1, $bodyText.Length - 2)))
        }
    }
}

function New-TestManifest {
    param(
        [Parameter(Mandatory = $true)][string] $Channel,
        [Parameter(Mandatory = $true)][string[]] $Paths,
        [Parameter(Mandatory = $true)][string] $Root,
        [hashtable] $HashOverrides = @{}
    )

    $files = foreach ($relativePath in $Paths) {
        $hash = if ($HashOverrides.ContainsKey($relativePath)) {
            [string] $HashOverrides[$relativePath]
        } else {
            Get-Sha256 -Path (Join-Path $Root ($relativePath -replace '/', '\'))
        }
        [PSCustomObject]@{
            path = $relativePath
            sha256 = $hash
        }
    }
    return [PSCustomObject]@{
        signing = [PSCustomObject]@{ channel = $Channel }
        files = @($files)
    }
}

function Assert-Accepted {
    param(
        [Parameter(Mandatory = $true)] $Manifest,
        [Parameter(Mandatory = $true)][string] $Root,
        [Parameter(Mandatory = $true)][string] $Name
    )

    try {
        Assert-ManifestPayloadContract -Manifest $Manifest -Root $Root
    } catch {
        throw "$Name was rejected: $($_.Exception.Message)"
    }
}

function Assert-Rejected {
    param(
        [Parameter(Mandatory = $true)] $Manifest,
        [Parameter(Mandatory = $true)][string] $Root,
        [Parameter(Mandatory = $true)][string] $Name,
        [Parameter(Mandatory = $true)][string] $ExpectedMessage
    )

    try {
        Assert-ManifestPayloadContract -Manifest $Manifest -Root $Root
    } catch {
        if ($_.Exception.Message.IndexOf($ExpectedMessage, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
            throw "$Name failed for the wrong reason: $($_.Exception.Message)"
        }
        return
    }
    throw "$Name was accepted."
}

$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) "vhf-manifest-contract-$([System.Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
try {
    $testFiles = @($manifestPayload) + @(
        $localTestCertificate,
        'driver/AlternateVhfGamepad.cer',
        'driver/UnexpectedVhfGamepad.cer'
    )
    foreach ($relativePath in $testFiles) {
        $path = Join-Path $testRoot ($relativePath -replace '/', '\')
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [System.IO.File]::WriteAllText($path, "fixture:$relativePath", [System.Text.UTF8Encoding]::new($false))
    }

    foreach ($target in @(
        [PSCustomObject]@{ Path = $refreshScript; Owner = 'Assert-Package'; Name = 'refresh validation' },
        [PSCustomObject]@{ Path = $installScript; Owner = 'Assert-DriverPackage'; Name = 'installed validation' }
    )) {
        Import-ManifestContract -ScriptPath $target.Path -OwnerFunction $target.Owner

        $localPaths = @($manifestPayload) + $localTestCertificate
        Assert-Accepted `
            -Manifest (New-TestManifest -Channel 'self-signed-local-test' -Paths $localPaths -Root $testRoot) `
            -Root $testRoot `
            -Name "$($target.Name): valid local-test manifest"

        Assert-Rejected `
            -Manifest (New-TestManifest -Channel 'self-signed-local-test' -Paths $manifestPayload -Root $testRoot) `
            -Root $testRoot `
            -Name "$($target.Name): missing certificate entry" `
            -ExpectedMessage 'manifest file list does not match'

        Assert-Rejected `
            -Manifest (New-TestManifest -Channel 'self-signed-local-test' -Paths ($localPaths + 'driver/UnexpectedVhfGamepad.cer') -Root $testRoot) `
            -Root $testRoot `
            -Name "$($target.Name): extra certificate entry" `
            -ExpectedMessage 'manifest file list does not match'

        $alternateCertificatePaths = @($manifestPayload) + 'driver/AlternateVhfGamepad.cer'
        Assert-Rejected `
            -Manifest (New-TestManifest -Channel 'self-signed-local-test' -Paths $alternateCertificatePaths -Root $testRoot) `
            -Root $testRoot `
            -Name "$($target.Name): mismatched certificate path" `
            -ExpectedMessage 'manifest file list does not match'

        Assert-Rejected `
            -Manifest (New-TestManifest `
                -Channel 'self-signed-local-test' `
                -Paths $localPaths `
                -Root $testRoot `
                -HashOverrides @{ 'driver/VibeshineVhfGamepad.cer' = ('0' * 64) }) `
            -Root $testRoot `
            -Name "$($target.Name): mismatched certificate hash" `
            -ExpectedMessage 'manifest hash mismatch'

        Assert-Accepted `
            -Manifest (New-TestManifest -Channel $msiRequestChannel -Paths $manifestPayload -Root $testRoot) `
            -Root $testRoot `
            -Name "$($target.Name): exact production manifest"

        Assert-Rejected `
            -Manifest (New-TestManifest -Channel $msiRequestChannel -Paths $localPaths -Root $testRoot) `
            -Root $testRoot `
            -Name "$($target.Name): production certificate entry" `
            -ExpectedMessage 'manifest file list does not match'
    }
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}

$expectedGate = @'
      - name: Test VHF manifest channel contract
        shell: pwsh
        run: .\packaging\windows\virtual_gamepad_driver\tests\test_manifest_channel_contract.ps1
'@

function Test-WorkflowContainsManifestContractGate {
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
    if (-not (Test-WorkflowContainsManifestContractGate `
            -WorkflowText $validGateFixture `
            -ExpectedGate $expectedGate)) {
        throw 'The manifest contract workflow gate rejected a valid newline style.'
    }
}
$missingGateFixture = $lfGateFixture.Replace(
    '.\packaging\windows\virtual_gamepad_driver\tests\test_manifest_channel_contract.ps1',
    '.\packaging\windows\virtual_gamepad_driver\tests\missing.ps1')
if (Test-WorkflowContainsManifestContractGate `
        -WorkflowText $missingGateFixture `
        -ExpectedGate $expectedGate) {
    throw 'The manifest contract workflow gate accepted a missing policy test.'
}

$workflowText = Get-Content -LiteralPath $workflowPath -Raw
if (-not (Test-WorkflowContainsManifestContractGate `
        -WorkflowText $workflowText `
        -ExpectedGate $expectedGate)) {
    throw 'The ordinary/release Windows build does not run this manifest contract test.'
}
$packagingText = Get-Content -LiteralPath $windowsPackagingCmake -Raw
if ($packagingText.IndexOf('test_manifest_channel_contract.ps1', [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
    throw 'The manifest contract test was added to the installed VHF payload.'
}
if (Test-Path -LiteralPath (Join-Path $repositoryRoot 'src_assets/windows/drivers/vhf-gamepad/tests/test_manifest_channel_contract.ps1')) {
    throw 'The manifest contract test is under the volatile installed payload root.'
}

Write-Host 'VHF manifest channel contract checks passed.'
