Start-Transcript -Path (Join-Path $PSScriptRoot 'trust-transcript.txt')
Write-Host ([Security.Principal.WindowsIdentity]::GetCurrent().Name)
$ErrorActionPreference = 'Stop'
Set-Location (Resolve-Path (Join-Path $PSScriptRoot '../..'))
Get-CimInstance Win32_OperatingSystem | Format-List Caption,Version,BuildNumber
& bcdedit.exe /enum '{current}'
try { Write-Host "SecureBoot=$(Confirm-SecureBootUEFI)" } catch { Write-Host $_ }
Get-ItemProperty 'HKLM:/SYSTEM/CurrentControlSet/Control/CI/Policy' -ErrorAction SilentlyContinue | Format-List

foreach ($driver in @('sunshine/SunshineVirtualDisplayDriver', 'sudovda/SudoVDA')) {
  $base = Join-Path $PWD "src_assets/windows/drivers/$driver"
  $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new([IO.File]::ReadAllBytes("$base.cer"))
  Write-Host "Testing $driver certificate $($certificate.Thumbprint)"
  $infPath = "$base.inf"; $catPath = "$base.cat"; $certPath = "$base.cer"; $dllPath = "$base.dll"
  $hardwareId = if ($driver.StartsWith('sunshine/')) { 'ROOT\SunshineVirtualDisplay' } else { 'ROOT\SUDOMAKER\SUDOVDA' }
  $sourcePath = Join-Path (Split-Path $base) 'install.ps1'
  $tokens = $null; $parseErrors = $null
  $ast = [Management.Automation.Language.Parser]::ParseFile($sourcePath, [ref]$tokens, [ref]$parseErrors)
  if ($parseErrors.Count) { throw 'Installer source does not parse' }
  $names = @('Initialize-DisplayPackageValidation','Assert-ExpectedDisplayPackage','Test-DisplayCertificatePresent','Add-DisplayCertificate','Remove-DisplayCertificate','Undo-DisplayTrustProvisioning','Install-ExpectedDisplayTrust')
  foreach ($name in $names) {
    $function = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }, $false)
    if (!$function) { throw "Missing original installer function: $name" }
    Invoke-Expression $function.Extent.Text
  }
  $null = Assert-ExpectedDisplayPackage
  Install-ExpectedDisplayTrust
  Get-AuthenticodeSignature "$base.cat" | Format-List Status,StatusMessage,SignerCertificate
  & pnputil.exe /add-driver "$base.inf"
  $code = $LASTEXITCODE
  Write-Host "PNPUTIL_EXIT=$code"
  Get-Content "$env:windir/inf/setupapi.dev.log" -Tail 250
  if ($code -ne 0 -and $code -ne 3010) { throw "Native staging failed: $driver ($code)" }
}
