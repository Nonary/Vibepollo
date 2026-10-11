param([Parameter(Mandatory=$true)][string]$PackageRoot)
$ErrorActionPreference = 'Stop'
$manifest = Get-Content -LiteralPath (Join-Path $PackageRoot 'manifest.json') -Raw | ConvertFrom-Json
$certificateBytes = [IO.File]::ReadAllBytes((Join-Path $PackageRoot 'driver/VibeshineVhfGamepad.cer'))
if ([Security.Cryptography.X509Certificates.X509Certificate2]::GetCertContentType($certificateBytes) -ne [Security.Cryptography.X509Certificates.X509ContentType]::Cert) {
    throw 'The public certificate must contain one raw certificate, never PKCS12/private-key material.'
}
$certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certificateBytes)
if ($certificate.HasPrivateKey -or $certificate.Subject -cne $certificate.Issuer -or $certificate.Subject -cne 'CN=Nonary Daily VHF Gamepad Test Certificate' -or $certificate.NotBefore.ToUniversalTime() -gt [DateTime]::UtcNow -or $certificate.NotAfter.ToUniversalTime() -lt [DateTime]::UtcNow) {
    throw 'Expected a current, self-issued public daily gamepad certificate with the pinned subject.'
}
$constraints = @($certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.19' })
$usages = @($certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' })
if ($constraints.Count -ne 1 -or $usages.Count -ne 1) { throw 'Explicit non-CA/code-signing constraints are required.' }
$basic = [Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($constraints[0], $constraints[0].Critical)
$eku = [Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($usages[0], $usages[0].Critical)
if ($basic.CertificateAuthority -or $eku.EnhancedKeyUsages.Count -ne 1 -or $eku.EnhancedKeyUsages[0].Value -cne '1.3.6.1.5.5.7.3.3') { throw 'Daily gamepad signer must be non-CA and restricted to code signing.' }
if ($manifest.signing.channel -cne 'self-signed-local-test' -or $certificate.Thumbprint -cne $manifest.signing.signer_thumbprint -or $certificate.Thumbprint -cne $manifest.signing.device_setup_signer_thumbprint) {
    throw 'Bundled certificate identity does not match the gamepad manifest.'
}
[ordered]@{thumbprint=$certificate.Thumbprint; subject=$certificate.Subject; driver_ver=$manifest.driver_ver} | ConvertTo-Json -Compress
