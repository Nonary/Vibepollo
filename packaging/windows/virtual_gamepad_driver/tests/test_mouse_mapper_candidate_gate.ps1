$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$downloader = Join-Path $repositoryRoot 'scripts/download_libvirtualgamepad_release.ps1'
$tokens=$null; $errors=$null
$ast = [Management.Automation.Language.Parser]::ParseFile($downloader, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Downloader parser failure.' }
foreach ($statement in $ast.EndBlock.Statements) {
    if ($statement -is [Management.Automation.Language.AssignmentStatementAst] -and
        $statement.Extent.Text -match '^\$(expectedLayout|expectedManifestFiles|expectedMembershipFiles|expectedUnsignedPayloads|legacyLayout|legacyManifestFiles|legacyMembershipFiles|legacyUnsignedPayloads)\s*=') {
        . ([scriptblock]::Create($statement.Extent.Text))
    }
}
foreach ($name in @('Get-ValidatedMouseMapperMetadata', 'Assert-MouseMapperMetadataMatch', 'Assert-MouseMapperInfIdentity', 'Set-ReleasePayloadContract')) {
    $definition = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }.GetNewClosure(), $true)
    . ([scriptblock]::Create($definition.Extent.Text))
}
$legacy = [pscustomobject]@{ schema_version=1 }
$RequireMouseMapper = $false
Set-ReleasePayloadContract -Metadata $legacy
$RequireMouseMapper = $true
$rejected = $false
try { Set-ReleasePayloadContract -Metadata $legacy } catch { $rejected = $_.Exception.Message -match 'requires the ordered mouse mapper' }
if (-not $rejected) { throw 'Production candidate gate accepted legacy producer metadata.' }
$modern = [pscustomobject]@{ features=[pscustomobject]@{mouse_mapper=1}; mouse_mapper=[pscustomobject]@{transport_version=1;vendor_id=0;product_id=0;kernel_signing='unsigned-producer'} }
Set-ReleasePayloadContract -Metadata $modern
$otherSidecar = $modern | ConvertTo-Json -Depth 5 | ConvertFrom-Json
$otherSidecar.mouse_mapper.product_id = 1
$rejected = $false
try { Assert-MouseMapperMetadataMatch -Expected $modern -Actual $otherSidecar -Context 'Sidecar fixture' } catch { $rejected = $_.Exception.Message -match 'metadata mismatch' }
if (-not $rejected) { throw 'Consumer accepted a sidecar-only mouse identity change.' }
foreach ($invalid in @(-1, 65536, '1.5', 'not-an-id')) {
    $otherSidecar.mouse_mapper.vendor_id = $invalid
    $rejected = $false
    try { $null = Get-ValidatedMouseMapperMetadata -Metadata $otherSidecar } catch { $rejected = $_.Exception.Message -match '0..65535' }
    if (-not $rejected) { throw 'Consumer accepted an invalid 16-bit mapper identity.' }
}
Assert-MouseMapperInfIdentity -Metadata $modern -InfContents 'Model=Mapper,HID\VID_0000&PID_0000&REV_0100&Col02'
$rejected = $false
try { Assert-MouseMapperInfIdentity -Metadata $modern -InfContents 'Model=Mapper,HID\VID_0001&PID_0000&REV_0100&Col02' } catch { $rejected = $_.Exception.Message -match 'INF identity' }
if (-not $rejected) { throw 'Consumer accepted mapper metadata that disagrees with its INF.' }
if ($expectedLayout -notcontains 'mouse_mapper/VibeshineVhfMouseMapper.sys') { throw 'Mapper candidate layout omitted its SYS.' }
$root = Join-Path ([IO.Path]::GetTempPath()) ('vhf-candidate-gate-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $contract = (Join-Path $repositoryRoot 'cmake/packaging/windows_virtual_gamepad_contract.cmake').Replace('\','/')
    $cmakeFile = Join-Path $root 'gate.cmake'
    @"
include("$contract")
if(SUNSHINE_REQUIRE_VHF_MOUSE_MAPPER_PACKAGE)
  message(FATAL_ERROR "Bundled UMDF candidate must not require a kernel mapper.")
endif()
"@ | Set-Content -LiteralPath $cmakeFile
    & cmake -P $cmakeFile
    if ($LASTEXITCODE -ne 0) { throw 'Candidate CMake default is not enforced.' }
    $workflow = Get-Content -LiteralPath (Join-Path $repositoryRoot '.github/workflows/ci-windows.yml') -Raw
    if ($workflow -match '-DSUNSHINE_REQUIRE_VHF_MOUSE_MAPPER_PACKAGE=ON') { throw 'Host CI requires a kernel mapper.' }
    foreach ($call in [regex]::Matches($workflow, '(?m)^\s+\.\\scripts\\download_libvirtualgamepad_release\.ps1 `\r?\n(?:[^\r\n]*`\r?\n)*[^\r\n]*')) {
        if ($call.Value -match '-RequireMouseMapper') { throw 'A host producer download requires a kernel mapper.' }
    }
    Write-Output 'UMDF candidate gate passed: production has no mapper dependency; historical mapper payload verification remains strict.'
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
