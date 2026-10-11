$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../sign_daily_gamepad.ps1') -FunctionsOnly
$root = Join-Path ([IO.Path]::GetTempPath()) ('daily-sign-metadata-test-' + [Guid]::NewGuid().ToString('N'))
try {
    $paths = @('driver/VibeshineVhfGamepad.cat', 'driver/VibeshineVhfGamepad.cer', 'driver/VibeshineVhfGamepad.dll', 'driver/VibeshineVhfGamepad.inf', 'tools/VibeshineVhfGamepadDeviceSetup.exe')
    foreach ($relative in $paths) {
        $path = Join-Path $root $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllText($path, "mock $relative")
    }
    $before = (Get-FileHash (Join-Path $root 'driver/VibeshineVhfGamepad.dll')).Hash
    $manifestPath = Join-Path $root 'manifest.json'
    [IO.File]::WriteAllText($manifestPath, '{"schema_version":1,"source_revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","driver_ver":"10/04/2026,0.1.0.48","protocol_version":2,"platform":"x64","files":[],"signing":{"channel":"msi-request-signing"}}')
    $producer = [ordered]@{repository='Nonary/libvirtualgamepad'; tag='v0.1.0-beta.7'; archive_sha256=('a' * 64); manifest_sha256=('b' * 64)}
    Update-DailyGamepadManifest -Root $root -Thumbprint ('A' * 40) -ProducerLock $producer
    $actual = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($actual.signing.channel -cne 'self-signed-local-test' -or $actual.signing.signer_thumbprint -cne ('A' * 40) -or $actual.signing.device_setup_signer_thumbprint -cne ('A' * 40) -or $actual.signing.producer_release.archive_sha256 -cne ('a' * 64)) { throw 'Daily signing channel, signer identities or producer archive audit was lost.' }
    if ((@($actual.files.path | Sort-Object) -join ',') -cne (($paths | Sort-Object) -join ',')) { throw 'Public certificate/payload manifest is not exact.' }
    foreach ($entry in $actual.files) {
        if ($entry.sha256 -cne (Get-FileHash (Join-Path $root $entry.path)).Hash.ToLowerInvariant()) { throw 'Daily payload hash is inconsistent.' }
    }
    if ($before -cne (Get-FileHash (Join-Path $root 'driver/VibeshineVhfGamepad.dll')).Hash) { throw 'Catalog-bound DLL changed.' }
    $blocked = $false
    try { Update-DailyGamepadManifest -Root $root -Thumbprint ('A' * 40) -ProducerLock $producer } catch { $blocked = $true }
    if (-not $blocked) { throw 'An already signed manifest can be reinterpreted as a producer.' }
    $scriptText = Get-Content (Join-Path $PSScriptRoot '../sign_daily_gamepad.ps1') -Raw
    if ($scriptText -match 'Import-Certificate|Import-PfxCertificate|Cert:\\|certutil.*(-addstore|-importpfx)') { throw 'Daily signing must not import certificates.' }
    # Execute the actual workflow expectation block against the normalized
    # consumer layout, catching slash and ordering mistakes in the MSI gate.
    $workflow = Get-Content (Join-Path $PSScriptRoot '../../.github/workflows/ci-windows.yml') -Raw
    $start = $workflow.IndexOf('          $expected = @(')
    $finish = $workflow.IndexOf('          $actual = @(', $start)
    if ($start -lt 0 -or $finish -le $start) { throw 'Workflow MSI expectation block is missing.' }
    $env:DAILY_UNSIGNED = 'true'
    Invoke-Expression ($workflow.Substring($start, $finish - $start))
    $consumerLayout = @($paths + 'cleanup.ps1' + 'install.ps1' + 'manifest.json' + 'release-lock.json' | Sort-Object)
    if (($expected -join ',') -cne ($consumerLayout -join ',')) { throw 'Daily MSI exact-layout gate does not match the public-certificate package.' }
    Write-Host 'Daily signing metadata contract passed (mock payloads; no signing or trust mutation executed).'
} finally {
    foreach ($relative in @($paths) + 'manifest.json') { Remove-Item -LiteralPath (Join-Path $root $relative) -Force -ErrorAction SilentlyContinue }
    foreach ($relative in @('driver', 'tools')) { Remove-Item -LiteralPath (Join-Path $root $relative) -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $root -Force -ErrorAction SilentlyContinue
}
