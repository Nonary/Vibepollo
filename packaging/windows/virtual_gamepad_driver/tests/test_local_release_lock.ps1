$ErrorActionPreference = 'Stop'

# Exercise the actual lock writer/validator with a local-test manifest that,
# like a producer-generated manifest, omits the production-only list.
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$scriptPath = Join-Path $repositoryRoot 'packaging/windows/virtual_gamepad_driver/refresh_driver_package.ps1'
$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref] $tokens, [ref] $errors)
if ($errors.Count) { throw "Could not parse refresh script: $($errors.Message -join '; ')" }
foreach ($name in @('Normalize-Hex', 'Resolve-RequiredPath', 'Assert-File', 'Get-Sha256', 'Assert-ExactList', 'Get-RequiredStringProperty', 'Write-ReleaseLock', 'Assert-ReleaseLock')) {
    $definitions = @($ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }, $true))
    if ($definitions.Count -ne 1) { throw "Expected one definition of $name" }
    $body = $definitions[0].Body.Extent.Text
    Set-Item -LiteralPath "function:script:$name" -Value ([scriptblock]::Create($body.Substring(1, $body.Length - 2)))
}

$releaseLockFile = 'release-lock.json'
$msiRequestChannel = 'msi-request-signing'
$ReleaseTag = ''
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$fixture = [IO.Path]::GetFullPath((Join-Path $tempRoot "vhf-local-lock-$([guid]::NewGuid().ToString('N'))"))
if (-not $fixture.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped the temporary directory' }
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
    foreach ($name in @('install.ps1', 'cleanup.ps1')) { [IO.File]::WriteAllText((Join-Path $fixture $name), "fixture:$name") }
    $thumbprint = 'A' * 40
    $manifest = [PSCustomObject]@{
        source_revision = 'B' * 40
        driver_ver = '10/29/2026,0.1.0.87'
        protocol_version = 2
        signing = [PSCustomObject]@{
            channel = 'self-signed-local-test'
            signer_thumbprint = $thumbprint
            device_setup_signer_thumbprint = $thumbprint
        }
        files = @([PSCustomObject]@{ path = 'driver/test.inf'; sha256 = 'C' * 64 })
    }
    [IO.File]::WriteAllText((Join-Path $fixture 'manifest.json'), ($manifest | ConvertTo-Json -Depth 4))
    Write-ReleaseLock -Root $fixture -Manifest $manifest -AllowLocalTest
    $lockPath = Join-Path $fixture $releaseLockFile
    $lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
    if ($null -eq $lock.signed_downstream -or @($lock.signed_downstream).Count -ne 0) {
        throw 'A local-test lock must contain an empty signed_downstream array.'
    }
    Assert-ReleaseLock -Root $fixture -Manifest $manifest -AllowLocalTest

    # A one-element empty string is not the same declaration as an empty list.
    $lock.signed_downstream = @('')
    [IO.File]::WriteAllText($lockPath, ($lock | ConvertTo-Json -Depth 5))
    $rejected = $false
    try { Assert-ReleaseLock -Root $fixture -Manifest $manifest -AllowLocalTest } catch { $rejected = $true }
    if (-not $rejected) { throw 'A downstream signing list containing an empty entry was accepted.' }

    $lock.signed_downstream = @('unexpected')
    [IO.File]::WriteAllText($lockPath, ($lock | ConvertTo-Json -Depth 5))
    $rejected = $false
    try { Assert-ReleaseLock -Root $fixture -Manifest $manifest -AllowLocalTest } catch { $rejected = $true }
    if (-not $rejected) { throw 'A tampered downstream signing list was accepted.' }

    # Production locks still require the exact nonempty downstream list.
    $ReleaseTag = 'v0.1.0-test'
    $ExpectedSourceRevision = 'B' * 40
    $ExpectedReleaseAssetSha256 = 'D' * 64
    $ExpectedDriverVer = $manifest.driver_ver
    $ExpectedProtocolVersion = 2
    $manifest.signing = [PSCustomObject]@{
        channel = $msiRequestChannel
        signed_downstream = @('driver/VibeshineVhfGamepad.cat', 'tools/VibeshineVhfGamepadDeviceSetup.exe')
    }
    [IO.File]::WriteAllText((Join-Path $fixture 'manifest.json'), ($manifest | ConvertTo-Json -Depth 4))
    Write-ReleaseLock -Root $fixture -Manifest $manifest
    Assert-ReleaseLock -Root $fixture -Manifest $manifest
    $lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
    $lock.signed_downstream = @('driver/VibeshineVhfGamepad.cat')
    [IO.File]::WriteAllText($lockPath, ($lock | ConvertTo-Json -Depth 5))
    $rejected = $false
    try { Assert-ReleaseLock -Root $fixture -Manifest $manifest } catch { $rejected = $true }
    if (-not $rejected) { throw 'A truncated production signing list was accepted.' }
} finally {
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}
Write-Host 'VHF local-test release lock checks passed.'
