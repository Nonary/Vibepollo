[CmdletBinding()]
param([string]$SourceRoot, [string]$OutRoot, [string]$OpenSslPath, [string]$SignToolPath, [switch]$FunctionsOnly)
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
    $manifest.files = @($paths | ForEach-Object { [ordered]@{path=$_; sha256=(Get-FileHash -LiteralPath (Join-Path $Root $_) -Algorithm SHA256).Hash.ToLowerInvariant()} })
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
}

if ($FunctionsOnly) { return }
foreach ($argument in @($SourceRoot, $OutRoot, $OpenSslPath)) {
    if ([string]::IsNullOrWhiteSpace($argument)) { throw 'SourceRoot, OutRoot and OpenSslPath are required.' }
}
$source = [IO.Path]::GetFullPath($SourceRoot)
$output = [IO.Path]::GetFullPath($OutRoot)
if ($source -eq $output -or (Test-Path -LiteralPath $output)) { throw 'Daily output must be a fresh directory independent of the verified producer.' }
$manifest = Get-Content -LiteralPath (Join-Path $source 'manifest.json') -Raw | ConvertFrom-Json
$producerLock = Get-Content -LiteralPath (Join-Path $source 'consumer-release-lock.json') -Raw | ConvertFrom-Json
if ($manifest.signing.channel -cne 'msi-request-signing' -or $producerLock.signing_channel -cne 'msi-request-signing') { throw 'Only the prevalidated pinned producer signing-request package can enter daily signing.' }
$expected = @('driver/VibeshineVhfGamepad.cat', 'driver/VibeshineVhfGamepad.dll', 'driver/VibeshineVhfGamepad.inf', 'tools/VibeshineVhfGamepadDeviceSetup.exe')
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
$secretRoot = Join-Path $env:RUNNER_TEMP ("daily-gamepad-key-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $secretRoot | Out-Null
try {
    foreach ($relative in @($expected) + 'manifest.json') {
        $destination = Join-Path $output $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $source $relative) -Destination $destination
    }
    $key = Join-Path $secretRoot 'key.pem'
    $pem = Join-Path $secretRoot 'public.pem'
    $pfx = Join-Path $secretRoot 'signer.pfx'
    $env:DAILY_GAMEPAD_PFX_PASSWORD = [Guid]::NewGuid().ToString('N')
    & $OpenSslPath req -x509 -newkey rsa:3072 -nodes -keyout $key -out $pem -days 3650 -subj '/CN=Nonary Daily VHF Gamepad Test Certificate' -addext 'basicConstraints=critical,CA:FALSE' -addext 'extendedKeyUsage=codeSigning' -addext 'keyUsage=digitalSignature' 2>$null
    if ($LASTEXITCODE -ne 0) { throw 'Ephemeral certificate generation failed.' }
    $cer = Join-Path $output 'driver/VibeshineVhfGamepad.cer'
    & $OpenSslPath x509 -in $pem -outform DER -out $cer
    if ($LASTEXITCODE -ne 0) { throw 'Public certificate export failed.' }
    & $OpenSslPath pkcs12 -export -inkey $key -in $pem -out $pfx -passout env:DAILY_GAMEPAD_PFX_PASSWORD
    if ($LASTEXITCODE -ne 0) { throw 'Ephemeral PFX generation failed.' }
    $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($cer)
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
    Remove-Item Env:DAILY_GAMEPAD_PFX_PASSWORD -ErrorAction SilentlyContinue
    foreach ($name in @('key.pem', 'public.pem', 'signer.pfx')) {
        $secretPath = Join-Path $secretRoot $name
        if (Test-Path -LiteralPath $secretPath) { Remove-Item -LiteralPath $secretPath -Force -ErrorAction Stop }
    }
    Remove-Item -LiteralPath $secretRoot -Force -ErrorAction Stop
}
