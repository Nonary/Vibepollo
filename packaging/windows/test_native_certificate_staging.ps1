Start-Transcript -Path (Join-Path $PSScriptRoot 'trust-transcript.txt')
Write-Host ([Security.Principal.WindowsIdentity]::GetCurrent().Name)
$ErrorActionPreference = 'Stop'
foreach ($driver in @('sunshine/SunshineVirtualDisplayDriver', 'sudovda/SudoVDA')) {
  $base = Join-Path $PWD "src_assets/windows/drivers/$driver"
  $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new([IO.File]::ReadAllBytes("$base.cer"))
  Write-Host "Testing $driver certificate $($certificate.Thumbprint)"
  foreach ($name in @('Root','TrustedPublisher')) {
    $store = [Security.Cryptography.X509Certificates.X509Store]::new($name, 'LocalMachine')
    try {
      $store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
      $store.Add($certificate)
      Write-Host "$name present: $($store.Certificates.Find('FindByThumbprint', $certificate.Thumbprint, $false).Count)"
    } finally { $store.Close() }
  }
  Get-AuthenticodeSignature "$base.cat" | Format-List Status,StatusMessage,SignerCertificate
  & pnputil.exe /add-driver "$base.inf"
  $code = $LASTEXITCODE
  Write-Host "PNPUTIL_EXIT=$code"
  Get-Content "$env:windir/inf/setupapi.dev.log" -Tail 250
  if ($code -ne 0 -and $code -ne 3010) { throw "Native staging failed: $driver ($code)" }
}
