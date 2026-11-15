$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$downloader = Join-Path $repositoryRoot 'scripts/download_libvirtualgamepad_release.ps1'
$tokens=$null; $errors=$null
$ast = [Management.Automation.Language.Parser]::ParseFile($downloader, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Downloader parser failure.' }
foreach ($statement in $ast.EndBlock.Statements) {
    if ($statement -is [Management.Automation.Language.AssignmentStatementAst] -and
        $statement.Extent.Text -match '^\$(expectedLayout|expectedManifestFiles|expectedMembershipFiles|legacyLayout|legacyManifestFiles|legacyMembershipFiles)\s*=') {
        . ([scriptblock]::Create($statement.Extent.Text))
    }
}
$definition = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Set-ReleasePayloadContract' }, $true)
. ([scriptblock]::Create($definition.Extent.Text))
$legacy = [pscustomobject]@{ schema_version=1 }
$RequireMouseMapper = $false
Set-ReleasePayloadContract -Metadata $legacy
$RequireMouseMapper = $true
$rejected = $false
try { Set-ReleasePayloadContract -Metadata $legacy } catch { $rejected = $_.Exception.Message -match 'requires the ordered mouse mapper' }
if (-not $rejected) { throw 'Production candidate gate accepted legacy producer metadata.' }
$modern = [pscustomobject]@{ features=[pscustomobject]@{mouse_mapper=1}; mouse_mapper=[pscustomobject]@{transport_version=1;kernel_signing='windows-kernel-policy'} }
Set-ReleasePayloadContract -Metadata $modern
if ($expectedLayout -notcontains 'mouse_mapper/VibeshineVhfMouseMapper.sys') { throw 'Mapper candidate layout omitted its SYS.' }
$root = Join-Path ([IO.Path]::GetTempPath()) ('vhf-candidate-gate-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $contract = (Join-Path $repositoryRoot 'cmake/packaging/windows_virtual_gamepad_contract.cmake').Replace('\','/')
    $cmakeFile = Join-Path $root 'gate.cmake'
    @"
include("$contract")
if(NOT SUNSHINE_REQUIRE_VHF_MOUSE_MAPPER_PACKAGE)
  message(FATAL_ERROR "Bundled candidate mapper requirement must default ON.")
endif()
"@ | Set-Content -LiteralPath $cmakeFile
    & cmake -P $cmakeFile
    if ($LASTEXITCODE -ne 0) { throw 'Candidate CMake default is not enforced.' }
    $workflow = Get-Content -LiteralPath (Join-Path $repositoryRoot '.github/workflows/ci-windows.yml') -Raw
    if ($workflow -notmatch '-DSUNSHINE_REQUIRE_VHF_MOUSE_MAPPER_PACKAGE=ON') { throw 'Host CI does not explicitly require the mapper.' }
    foreach ($call in [regex]::Matches($workflow, '(?m)^\s+\.\\scripts\\download_libvirtualgamepad_release\.ps1 `\r?\n(?:[^\r\n]*`\r?\n)*[^\r\n]*')) {
        if ($call.Value -notmatch '-RequireMouseMapper') { throw 'A host producer download/revalidation call omitted the candidate gate.' }
    }
    Write-Output 'Mapper candidate gate passed: default ON, explicit CI requirement, legacy metadata rejected for candidates and retained for verification utilities.'
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
