param([string]$RepositoryRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
$FixtureRoot = Join-Path $RepositoryRoot 'src_assets/windows/drivers/sudovda'
$ErrorActionPreference='Stop'
# Extract only the read-only initializer; never dot-source or execute the installer body.
$source=Join-Path $RepositoryRoot 'src_assets/windows/drivers/vhf-gamepad/install.ps1'
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors | Out-String)}
$initializer=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Initialize-LocalGamepadTrustValidation'},$true)
. ([scriptblock]::Create($initializer.Extent.Text))
Initialize-LocalGamepadTrustValidation
$getter=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Get-PublicLocalGamepadCertificate'},$true)
. ([scriptblock]::Create($getter.Extent.Text))
$publicCertificate=Get-PublicLocalGamepadCertificate -CertificatePath (Join-Path $FixtureRoot 'SudoVDA.cer')
if($publicCertificate.HasPrivateKey){throw 'Public certificate reader must not load a private key'}
$wrongContentRejected=$false
try{$null=Get-PublicLocalGamepadCertificate -CertificatePath (Join-Path $FixtureRoot 'SudoVDA.cat')}catch{$wrongContentRejected=$true}
if(!$wrongContentRejected){throw 'Public certificate reader must reject PKCS#7/catalog content before X509 construction'}
$catalog=Join-Path $FixtureRoot 'SudoVDA.cat'; $inf=Join-Path $FixtureRoot 'SudoVDA.inf'; $dll=Join-Path $FixtureRoot 'SudoVDA.dll'
$signature=Get-AuthenticodeSignature -LiteralPath $catalog
if ($null -eq $signature.SignerCertificate) { throw 'Existing fixture must be signed; no keys will be generated.' }
$thumbprint=$signature.SignerCertificate.Thumbprint
[VhfLocalTrustValidation]::VerifyCatalog($catalog,$thumbprint,@($inf,$dll))
$tool=Join-Path $FixtureRoot 'nefconc.exe'
$toolThumbprint=(Get-AuthenticodeSignature -LiteralPath $tool).SignerCertificate.Thumbprint
[VhfLocalTrustValidation]::VerifyTool($tool,$toolThumbprint)
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$scratch=Join-Path $tempRoot ('vhf-native-validation-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
 function Expect-CatalogRejected([string]$Cat,[string]$Thumb,[string[]]$Members,[string]$Case) {
  $failed=$false; try { [VhfLocalTrustValidation]::VerifyCatalog($Cat,$Thumb,$Members) } catch {$failed=$true}
  if (!$failed) {throw "Native catalog check accepted $Case"}
 }
 $infCopy=Join-Path $scratch 'SudoVDA.inf'; [IO.File]::WriteAllText($infCopy,[IO.File]::ReadAllText($inf)+"`r`n; modified fixture`r`n")
 Expect-CatalogRejected $catalog $thumbprint @($infCopy,$dll) 'tampered INF'
 $dllCopy=Join-Path $scratch 'SudoVDA.dll'; $bytes=[IO.File]::ReadAllBytes($dll); $bytes[[int]($bytes.Length/2)]=$bytes[[int]($bytes.Length/2)] -bxor 1; [IO.File]::WriteAllBytes($dllCopy,$bytes)
 Expect-CatalogRejected $catalog $thumbprint @($inf,$dllCopy) 'tampered driver DLL'
 $catCopy=Join-Path $scratch 'SudoVDA.cat'; $bytes=[IO.File]::ReadAllBytes($catalog); $bytes[$bytes.Length-12]=$bytes[$bytes.Length-12] -bxor 1; [IO.File]::WriteAllBytes($catCopy,$bytes)
 Expect-CatalogRejected $catCopy $thumbprint @($inf,$dll) 'tampered catalog'
 Expect-CatalogRejected $catalog ('0'*40) @($inf,$dll) 'wrong certificate identity'
 $toolCopy=Join-Path $scratch 'nefconc.exe'; $bytes=[IO.File]::ReadAllBytes($tool); $bytes[[int]($bytes.Length/2)]=$bytes[[int]($bytes.Length/2)] -bxor 1; [IO.File]::WriteAllBytes($toolCopy,$bytes)
 $failed=$false; try {[VhfLocalTrustValidation]::VerifyTool($toolCopy,$toolThumbprint)} catch {$failed=$true}; if (!$failed) {throw 'Native tool check accepted tampered executable'}
 Write-Output 'Read-only native CMS/catalog validation: existing signed INF+DLL accepted; tampered INF/DLL/CAT/tool and wrong signer rejected. No trust, driver, key or service mutation executed.'
} finally {
 $resolved=[IO.Path]::GetFullPath((Resolve-Path -LiteralPath $scratch).Path)
 if (!$resolved.Equals([IO.Path]::GetFullPath($scratch),[StringComparison]::OrdinalIgnoreCase) -or
     !$resolved.StartsWith($tempRoot.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
     [IO.Path]::GetFileName($resolved) -notmatch '^vhf-native-validation-[0-9a-f]{32}$') {throw 'Unsafe native test scratch cleanup path'}
 Remove-Item -LiteralPath $resolved -Recurse -Force
}
