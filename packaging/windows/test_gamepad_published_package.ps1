param(
 [Parameter(Mandatory=$true)][string]$FixtureRoot,
 [string]$RepositoryRoot
)
$ErrorActionPreference='Stop'
if(!$RepositoryRoot){$RepositoryRoot=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)}
# The fixture is a normalized extraction of an already signed public package.
# Never load the installer body, generate keys, or call trust/install functions.
$source=Join-Path $RepositoryRoot 'src_assets/windows/drivers/vhf-gamepad/install.ps1'
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors | Out-String)}
foreach($statement in $ast.EndBlock.Statements) {
 if($statement -is [Management.Automation.Language.FunctionDefinitionAst]){break}
 if($statement -is [Management.Automation.Language.AssignmentStatementAst]){. ([scriptblock]::Create($statement.Extent.Text))}
}
foreach($fn in $ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst]},$false)) {
 . ([scriptblock]::Create($fn.Extent.Text))
}
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$scratch=Join-Path $tempRoot ('vhf-published-validation-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
 Copy-Item -Path (Join-Path $FixtureRoot '*') -Destination $scratch -Recurse
 # A future package refresh binds the corrected script into its new lock.
 # Fixture payload/signatures remain the exact previously published bytes.
 Copy-Item -LiteralPath $source -Destination (Join-Path $scratch 'install.ps1')
 $lockPath=Join-Path $scratch 'release-lock.json'
 $lock=Get-Content $lockPath -Raw | ConvertFrom-Json
 $lock.installer_script_sha256.'install.ps1'=Get-Sha256 (Join-Path $scratch 'install.ps1')
 $lock | ConvertTo-Json -Depth 12 | Set-Content $lockPath -Encoding UTF8
 $validated=Assert-DriverPackage -Root $scratch -AllowUntrustedLocalPackage
 if($null -eq $validated.LocalCertificate){throw 'Local package must be independently verified'}
 function Expect-Rejected([scriptblock]$Action,[string]$Case) {
  $failed=$false;try{& $Action}catch{$failed=$true}
  if(!$failed){throw "Real package validation accepted $Case"}
 }
 $catalogStatus=(Get-AuthenticodeSignature -LiteralPath $validated.CatalogPath).Status
 $toolStatus=(Get-AuthenticodeSignature -LiteralPath $validated.SetupToolPath).Status
 if($catalogStatus -ne 'Valid' -or $toolStatus -ne 'Valid') {
  Expect-Rejected {Assert-DriverPackage -Root $scratch} 'untrusted package without independent verification'
 } else {
  $alreadyTrusted=Assert-DriverPackage -Root $scratch
  if($null -ne $alreadyTrusted.LocalCertificate){throw 'Existing trust must not request new local trust'}
 }
 foreach($relative in @('driver/VibeshineVhfGamepad.inf','driver/VibeshineVhfGamepad.dll','driver/VibeshineVhfGamepad.cat','tools/VibeshineVhfGamepadDeviceSetup.exe')) {
  $path=Join-Path $scratch $relative;$original=[IO.File]::ReadAllBytes($path)
  $changed=$original.Clone();$changed[[int]($changed.Length/2)]=$changed[[int]($changed.Length/2)] -bxor 1
  [IO.File]::WriteAllBytes($path,$changed)
  Expect-Rejected {Assert-DriverPackage -Root $scratch -AllowUntrustedLocalPackage} "tampered $relative"
  [IO.File]::WriteAllBytes($path,$original)
 }
 $certificatePath=Join-Path $scratch 'driver/VibeshineVhfGamepad.cer'
 $certificateBytes=[IO.File]::ReadAllBytes($certificatePath)
 Remove-Item -LiteralPath $certificatePath
 Expect-Rejected {Assert-DriverPackage -Root $scratch -AllowUntrustedLocalPackage} 'missing certificate'
 [IO.File]::WriteAllBytes($certificatePath,$certificateBytes)
 $lock.catalog_signer_thumbprint='0'*40;$lock | ConvertTo-Json -Depth 12 | Set-Content $lockPath -Encoding UTF8
 Expect-Rejected {Assert-DriverPackage -Root $scratch -AllowUntrustedLocalPackage} 'wrong pinned identity'
 Write-Output "Real published package passed independent validation (CAT=$catalogStatus, tool=$toolStatus); trust-status control, tampered INF/DLL/CAT/tool, missing certificate and wrong pin passed. No trust or installation executed."
} finally {
 $resolved=[IO.Path]::GetFullPath((Resolve-Path -LiteralPath $scratch).Path)
 if(!$resolved.StartsWith($tempRoot.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^vhf-published-validation-[0-9a-f]{32}$'){throw 'Unsafe fixture cleanup path'}
 Remove-Item -LiteralPath $resolved -Recurse -Force
}
