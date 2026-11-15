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
        if ($manifest.mouse_mapper.kernel_signing -notin @('unsigned-producer', 'windows-kernel-policy')) { throw 'Unsupported producer mapper signing metadata.' }
        $paths += @('mouse_mapper/VibeshineVhfMouseMapper.inf', 'mouse_mapper/VibeshineVhfMouseMapper.sys', 'mouse_mapper/VibeshineVhfMouseMapper.cat')
    }
    $manifest.files = @($paths | ForEach-Object { [ordered]@{path=$_; sha256=(Get-FileHash -LiteralPath (Join-Path $Root $_) -Algorithm SHA256).Hash.ToLowerInvariant()} })
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
}

if ($FunctionsOnly) { return }
foreach ($argument in @($SourceRoot, $OutRoot, $PfxPath, $ExpectedSignerThumbprint, $env:DAILY_GAMEPAD_PFX_PASSWORD)) {
    if ([string]::IsNullOrWhiteSpace($argument)) { throw 'SourceRoot, OutRoot, PfxPath, ExpectedSignerThumbprint and the signing password environment variable are required.' }
}
$source = [IO.Path]::GetFullPath($SourceRoot)
$output = [IO.Path]::GetFullPath($OutRoot)
if ($source -eq $output -or (Test-Path -LiteralPath $output)) { throw 'Daily output must be a fresh directory independent of the verified producer.' }
$manifest = Get-Content -LiteralPath (Join-Path $source 'manifest.json') -Raw | ConvertFrom-Json
$producerLock = Get-Content -LiteralPath (Join-Path $source 'consumer-release-lock.json') -Raw | ConvertFrom-Json
if ($manifest.signing.channel -cne 'msi-request-signing' -or $producerLock.signing_channel -cne 'msi-request-signing') { throw 'Only the prevalidated pinned producer signing-request package can enter daily signing.' }
$expected = @('driver/VibeshineVhfGamepad.cat', 'driver/VibeshineVhfGamepad.dll', 'driver/VibeshineVhfGamepad.inf', 'tools/VibeshineVhfGamepadDeviceSetup.exe')
if ($null -ne $manifest.PSObject.Properties['features'] -and $manifest.features.mouse_mapper -eq 1) {
    if ($manifest.mouse_mapper.kernel_signing -notin @('unsigned-producer', 'windows-kernel-policy')) { throw 'Unsupported producer mapper signing metadata.' }
    $expected = @($expected + @('mouse_mapper/VibeshineVhfMouseMapper.cat', 'mouse_mapper/VibeshineVhfMouseMapper.inf', 'mouse_mapper/VibeshineVhfMouseMapper.sys') | Sort-Object)
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
if ($expected -contains 'mouse_mapper/VibeshineVhfMouseMapper.sys' -and $manifest.mouse_mapper.kernel_signing -ceq 'windows-kernel-policy') {
    foreach ($relative in @('mouse_mapper/VibeshineVhfMouseMapper.inf', 'mouse_mapper/VibeshineVhfMouseMapper.sys')) {
        & $SignToolPath verify '/v' '/kp' '/c' (Join-Path $source 'mouse_mapper/VibeshineVhfMouseMapper.cat') (Join-Path $source $relative)
        if ($LASTEXITCODE -ne 0) { throw "Producer mapper kernel policy/membership failed: $relative" }
    }
}
if ($expected -contains 'mouse_mapper/VibeshineVhfMouseMapper.sys' -and $manifest.mouse_mapper.kernel_signing -ceq 'unsigned-producer') {
    foreach ($relative in @('mouse_mapper/VibeshineVhfMouseMapper.cat', 'mouse_mapper/VibeshineVhfMouseMapper.sys')) {
        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $source $relative)
        if ($signature.Status -ne 'NotSigned' -or $null -ne $signature.SignerCertificate) { throw "Unsigned producer mapper was rewritten/signed: $relative" }
    }
}
if ($ExpectedSignerThumbprint -cnotmatch '^[0-9A-F]{40}$') { throw 'Expected signer identity must be explicitly pinned.' }
$certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new(
    [IO.File]::ReadAllBytes([IO.Path]::GetFullPath($PfxPath)),
    $env:DAILY_GAMEPAD_PFX_PASSWORD,
    [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
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
    $pfx = [IO.Path]::GetFullPath($PfxPath)
    $cer = Join-Path $output 'driver/VibeshineVhfGamepad.cer'
    [IO.File]::WriteAllBytes($cer, $certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    foreach ($relative in @('driver/VibeshineVhfGamepad.cat', 'tools/VibeshineVhfGamepadDeviceSetup.exe')) {
        $path = Join-Path $output $relative
        & $SignToolPath sign /fd SHA256 /f $pfx /p $env:DAILY_GAMEPAD_PFX_PASSWORD $path
        if ($LASTEXITCODE -ne 0) { throw "Daily signature failed: $relative" }
        $signature = Get-AuthenticodeSignature -LiteralPath $path
        if ($null -eq $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -cne $certificate.Thumbprint -or $signature.Status -eq 'HashMismatch') { throw "Daily signature identity/integrity failed: $relative" }
    }
    # Never rewrite catalog-bound INF/DLL: preserve the verified producer membership.
    foreach ($relative in @('driver/VibeshineVhfGamepad.inf', 'driver/VibeshineVhfGamepad.dll')) {
        if ((Get-FileHash (Join-Path $source $relative)).Hash -cne (Get-FileHash (Join-Path $output $relative)).Hash) { throw "Catalog-bound payload changed: $relative" }
    }
    Update-DailyGamepadManifest -Root $output -Thumbprint $certificate.Thumbprint -ProducerLock $producerLock
    Write-Host "Daily VHF catalog/setup self-signed. Public certificate: $($certificate.Thumbprint). No trust store was changed."
 } finally {
    $certificate.Dispose()
}
