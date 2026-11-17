[CmdletBinding()]
param([string]$SourceRoot, [string]$OutRoot, [string]$PfxPath, [string]$ExpectedSignerThumbprint, [string]$SignToolPath, [switch]$FunctionsOnly)
$ErrorActionPreference = 'Stop'

function Update-DailyGamepadManifest {
    param([string]$Root, [string]$Thumbprint, $ProducerLock)
    $manifestPath = Join-Path $Root 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.signing.channel -cne 'msi-request-signing' -or $Thumbprint -notmatch '^[0-9A-F]{40}$') {
        throw 'Daily signing requires a verified unsigned producer and exact certificate identity.'
    }
    $manifest.signing = [ordered]@{
        channel = 'self-signed-local-test'
        signer_thumbprint = $Thumbprint
        device_setup_signer_thumbprint = $Thumbprint
        signed_downstream = @()
        unsigned_payloads = @('driver/VibeshineVhfGamepad.dll')
        producer_release = $ProducerLock
        trust = 'Manual OS security review required; no automatic certificate import'
    }
    $paths = @('driver/VibeshineVhfGamepad.cat', 'driver/VibeshineVhfGamepad.cer', 'driver/VibeshineVhfGamepad.dll', 'driver/VibeshineVhfGamepad.inf', 'tools/VibeshineVhfGamepadDeviceSetup.exe')
    if ($null -ne $manifest.PSObject.Properties['features'] -and $manifest.features.mouse_mapper -eq 1) {
        throw 'The nightly uses the UMDF-only producer; kernel mouse mapper payloads are not supported.'
    }
    $manifest.files = @($paths | ForEach-Object { [ordered]@{path=$_; sha256=(Get-FileHash -LiteralPath (Join-Path $Root $_) -Algorithm SHA256).Hash.ToLowerInvariant()} })
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
}

if ($FunctionsOnly) { return }
foreach ($argument in @($SourceRoot, $OutRoot, $ExpectedSignerThumbprint)) {
    if ([string]::IsNullOrWhiteSpace($argument)) { throw 'SourceRoot, OutRoot and ExpectedSignerThumbprint are required.' }
}
if ($PfxPath -and -not $env:DAILY_GAMEPAD_PFX_PASSWORD) { throw 'A PFX requires the signing password environment variable.' }
$source = [IO.Path]::GetFullPath($SourceRoot)
$output = [IO.Path]::GetFullPath($OutRoot)
if ($source -eq $output -or (Test-Path -LiteralPath $output)) { throw 'Daily output must be a fresh directory independent of the verified producer.' }
$manifest = Get-Content -LiteralPath (Join-Path $source 'manifest.json') -Raw | ConvertFrom-Json
$producerLock = Get-Content -LiteralPath (Join-Path $source 'consumer-release-lock.json') -Raw | ConvertFrom-Json
if ($manifest.signing.channel -cne 'msi-request-signing' -or $producerLock.signing_channel -cne 'msi-request-signing') { throw 'Only the prevalidated pinned producer signing-request package can enter daily signing.' }
$expected = @('driver/VibeshineVhfGamepad.cat', 'driver/VibeshineVhfGamepad.dll', 'driver/VibeshineVhfGamepad.inf', 'tools/VibeshineVhfGamepadDeviceSetup.exe')
if ($null -ne $manifest.PSObject.Properties['features'] -and $manifest.features.mouse_mapper -eq 1) {
    throw 'The nightly uses the UMDF-only producer; kernel mouse mapper payloads are not supported.'
}
if ((@($manifest.files.path | Sort-Object) -join ',') -cne ($expected -join ',')) { throw 'Producer payload list is not exact.' }
foreach ($entry in $manifest.files) {
    if ((Get-FileHash -LiteralPath (Join-Path $source $entry.path) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) { throw "Producer hash changed before signing: $($entry.path)" }
}
if ((Get-FileHash -LiteralPath (Join-Path $source 'manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant() -cne $producerLock.manifest_sha256) { throw 'Producer manifest changed before signing.' }
if (-not $SignToolPath) {
    $kit = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
    $SignToolPath = Get-ChildItem -LiteralPath $kit -Directory | Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'x64/signtool.exe' } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $SignToolPath) { throw 'Windows SDK x64 SignTool is required.' }
if ($ExpectedSignerThumbprint -cnotmatch '^[0-9A-F]{40}$') { throw 'Expected signer identity must be explicitly pinned.' }
$certificate = if ($PfxPath) {
    [Security.Cryptography.X509Certificates.X509Certificate2]::new(
        [IO.File]::ReadAllBytes([IO.Path]::GetFullPath($PfxPath)), $env:DAILY_GAMEPAD_PFX_PASSWORD,
        [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
} else {
    # Local validation uses the same signer pipeline with an explicitly pinned
    # existing key; never select by subject or silently generate a replacement.
    Get-Item -LiteralPath "Cert:\CurrentUser\My\$ExpectedSignerThumbprint" -ErrorAction Stop
}
try {
    if (-not $certificate.HasPrivateKey -or $certificate.Thumbprint -cne $ExpectedSignerThumbprint -or
        $certificate.NotBefore.ToUniversalTime() -gt [DateTime]::UtcNow -or
        $certificate.NotAfter.ToUniversalTime() -le [DateTime]::UtcNow.AddDays(30)) {
        throw 'Reusable signing identity is missing, mismatched, not yet valid, or near expiry.'
    }
    $eku = @($certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' })
    if ($eku.Count -ne 1 -or '1.3.6.1.5.5.7.3.3' -notin @($eku[0].EnhancedKeyUsages.Value)) { throw 'Reusable identity must permit code signing.' }
    foreach ($relative in @($expected) + 'manifest.json') {
        $destination = Join-Path $output $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $source $relative) -Destination $destination
    }
    function Sign-DailyFile([string]$RelativePath) {
        $path = Join-Path $output $RelativePath
        $keyArguments = if ($PfxPath) { @('/f', [IO.Path]::GetFullPath($PfxPath), '/p', $env:DAILY_GAMEPAD_PFX_PASSWORD) } else { @('/sha1', $ExpectedSignerThumbprint) }
        & $SignToolPath sign /fd SHA256 @keyArguments $path
        if ($LASTEXITCODE -ne 0) { throw "Daily signature failed: $RelativePath" }
        $signature = Get-AuthenticodeSignature -LiteralPath $path
        if ($null -eq $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -cne $certificate.Thumbprint -or $signature.Status -eq 'HashMismatch') { throw "Daily signature identity/integrity failed: $RelativePath" }
    }
    $cer = Join-Path $output 'driver/VibeshineVhfGamepad.cer'
    [IO.File]::WriteAllBytes($cer, $certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    foreach ($relative in @('driver/VibeshineVhfGamepad.cat', 'tools/VibeshineVhfGamepadDeviceSetup.exe')) {
        Sign-DailyFile $relative
    }
    # Never rewrite catalog-bound INF/DLL: preserve the verified producer membership.
    $unchanged = @('driver/VibeshineVhfGamepad.inf', 'driver/VibeshineVhfGamepad.dll')
    foreach ($relative in $unchanged) {
        if ((Get-FileHash (Join-Path $source $relative)).Hash -cne (Get-FileHash (Join-Path $output $relative)).Hash) { throw "Catalog-bound payload changed: $relative" }
    }
    Update-DailyGamepadManifest -Root $output -Thumbprint $certificate.Thumbprint -ProducerLock $producerLock
    # Reuse the installer's exact offline integrity/CMS/membership verifier.
    . (Join-Path $PSScriptRoot '../src_assets/windows/drivers/vhf-gamepad/install.ps1') -FunctionsOnly
    Initialize-LocalGamepadTrustValidation
    [VhfLocalTrustValidation]::VerifyCatalog((Join-Path $output 'driver/VibeshineVhfGamepad.cat'), $certificate.Thumbprint, @((Join-Path $output 'driver/VibeshineVhfGamepad.inf'), (Join-Path $output 'driver/VibeshineVhfGamepad.dll')))
    [VhfLocalTrustValidation]::VerifyTool((Join-Path $output 'tools/VibeshineVhfGamepadDeviceSetup.exe'), $certificate.Thumbprint)
    Write-Host "Daily VHF package signatures and catalog membership verified. Public certificate: $($certificate.Thumbprint). No trust store or boot setting was changed."
 } finally {
    $certificate.Dispose()
}
