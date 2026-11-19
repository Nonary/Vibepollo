param([string]$RepositoryRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
$ErrorActionPreference='Stop'
function Assert-Check([bool]$Condition,[string]$Message) { if (!$Condition) {throw $Message} }
$path=Join-Path $RepositoryRoot 'src_assets/windows/drivers/vhf-gamepad/install.ps1'
$tokens=$null; $errors=$null; $ast=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
Assert-Check ($errors.Count -eq 0) 'VHF installer syntax must be valid'
foreach ($name in @('ConvertTo-DriverComponentVersion','Test-SafeDriverInstall','Get-Thumbprint','Assert-LocalTestCertificateIdentity','Ensure-ConsentedLocalGamepadTrust','Remove-AddedLocalGamepadTrust','Assert-MapperInstallCompatibility','Test-MapperParentInstallRequired','Install-DriverPackage')) {
 $function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name}.GetNewClosure(),$true)
 Assert-Check ($null -ne $function) "Missing tested function $name"
 . ([scriptblock]::Create($function.Extent.Text))
}
$script:certificate=[pscustomobject]@{Thumbprint='7F1868BF86698A8674F40E686B02C8C702BD3B88';Subject='CN=Fixture VHF';Issuer='CN=Fixture VHF'}
function Reset-Case {
 $script:stores=@{Root=@();TrustedPublisher=@()}; $script:imports=@(); $script:removals=@(); $script:storeReads=0
 $script:parentStoreVersion=$null
 $script:mapperDeclared=$false;$script:mapperInstalledVersion=$null;$script:mapperStoreVersion=$null;$script:mapperVendorId=43981
 $script:mapperInf=$null; $script:mapperExit=0; $script:mapperOnlyFailure=$false; $script:events=@()
 $script:installed=$null; $script:queryFailure=$false; $script:validationFailure=$false; $script:stagingFailure=$false; $script:postTrustFailure=$false; $script:importFailure=$false
 $script:validationCalls=0; $script:stagingCalls=0; $script:setupCalls=0; $script:untrustedStatus='UnknownError'; $script:setupFailure=$false
 $script:AllowLocalGamepadCertificateTrust=0; $script:ConsentCertificateThumbprint='none'; $script:DailyUnsignedPolicy=1; $script:rebootExitCode=3010
}
function Assert-Administrator {}
function Join-Path {param([AllowEmptyString()][string]$Path,[string]$ChildPath);return ('mock-root/'+$ChildPath)}
function Get-InfComponentVersion {param($Path) if($Path -ceq '/store/VibeshineVhfMouseMapper.inf'){return [version]$script:mapperStoreVersion};if($Path -ceq '/store/VibeshineVhfGamepad.inf'){return [version]$script:parentStoreVersion};return [version]'1.2.0.10'}
function Get-MapperHardwareId {param($InfPath) return 'HID\VID_ABCD&PID_5678&REV_0100&COL02'}
function Get-MapperInstallDeclaration {param($Root) if($script:mapperDeclared -or $null -ne $script:mapperInf){return [pscustomobject]@{mouse_mapper=[pscustomobject]@{transport_version=1;vendor_id=$script:mapperVendorId;product_id=22136;kernel_signing='self-signed-local-test'}}};return $null}
function Test-Path {param($LiteralPath) return $true}
function Get-WindowsDriver {
 param([switch]$Online,[switch]$All,$ErrorAction)
 if($script:queryFailure){throw 'Mock inventory failure'}
 $packages=@()
 if($null -ne $script:mapperStoreVersion){$packages += [pscustomobject]@{OriginalFileName='/store/VibeshineVhfMouseMapper.inf';Driver='oem1.inf'}}
 if($null -ne $script:parentStoreVersion){$packages += [pscustomobject]@{OriginalFileName='/store/VibeshineVhfGamepad.inf';Driver='oem2.inf'}}
 return $packages
}
function Get-CimInstance {
 param($ClassName,$ErrorAction)
 $devices=@()
 if($null -ne $script:installed){$devices += [pscustomobject]@{DeviceID='ROOT\VIBESHINEVIRTUALGAMEPAD\0000';HardwareID='ROOT\VIBESHINEVIRTUALGAMEPAD';DriverVersion=$script:installed}}
 if($null -ne $script:mapperInstalledVersion){$devices += [pscustomobject]@{DeviceID='HID\VID_ABCD&PID_5678&REV_0100&COL02\0000';HardwareID='HID\VID_ABCD&PID_5678&REV_0100&COL02';DriverVersion=$script:mapperInstalledVersion}}
 return $devices
}
function Get-AuthenticodeSignature {
 param($LiteralPath,$ErrorAction)
 $trusted=$script:stores.Root -contains $script:certificate.Thumbprint -and $script:stores.TrustedPublisher -contains $script:certificate.Thumbprint
 $status=if($trusted -and $script:postTrustFailure){'HashMismatch'}elseif($trusted){'Valid'}else{$script:untrustedStatus}
 return [pscustomobject]@{Status=$status;SignerCertificate=$script:certificate}
}
function Assert-DriverPackage {
 param($Root,[switch]$AllowUntrustedLocalPackage,[switch]$DeferMapper)
 $script:validationCalls++
 Assert-Check $AllowUntrustedLocalPackage 'Local validation must be explicit and independent of trust'
 Assert-Check $DeferMapper 'Base validation must isolate optional mapper validation'
 if($script:validationFailure){throw 'Mock cryptographic binding/payload tamper rejection'}
 return [pscustomobject]@{MapperInfPath=$null;InfPath='mock.inf';CatalogPath='mock.cat';SetupToolPath='Invoke-MockSetup';LocalCertificate=$script:certificate;ProductionPublisherCertificate=$null}
}
function Get-ChildItem {
 param($LiteralPath,$ErrorAction)
 $script:storeReads++; $name=Split-Path $LiteralPath -Leaf
 return @($script:stores[$name] | ForEach-Object {[pscustomobject]@{Thumbprint=$_}})
}
function Assert-OptionalMapperPackage {
 param($Package)
 if($null -eq $script:mapperInf){throw 'Missing mapper payload'}
 Assert-MapperInstallCompatibility -InfPath $script:mapperInf -Manifest (Get-MapperInstallDeclaration '')
 return $script:mapperInf
}
function Add-ValidatedLocalGamepadCertificate {
 param($Certificate,$StoreName)
 if($script:importFailure -and $StoreName -eq 'TrustedPublisher'){throw 'Mock publisher addition failure'}
 if($script:stores[$StoreName] -contains $Certificate.Thumbprint){return $false}
 $script:stores[$StoreName]+=$Certificate.Thumbprint; $script:imports+=$StoreName; return $true
}
function Remove-ValidatedLocalGamepadCertificate {
 param($Certificate,$StoreName)
 $script:stores[$StoreName]=@($script:stores[$StoreName] | Where-Object {$_ -cne $Certificate.Thumbprint}); $script:removals+=$StoreName
}
function Write-DriverMessage {param($Message)}
function Invoke-PnpUtil {param($Arguments);$script:stagingCalls++;$script:events+=@($Arguments[1]);if($script:stagingFailure -or ($script:mapperOnlyFailure -and $Arguments[1] -ceq 'mapper.inf')){throw 'Mock staging failure'};if($Arguments[1] -ceq 'mapper.inf'){return $script:mapperExit};return 0}
function Invoke-MockSetup {$script:events+=@('setup');$script:setupCalls++;$global:LASTEXITCODE=if($script:setupFailure){1}else{0}}
function Get-PublicLocalGamepadCertificate {param($CertificatePath);return $script:certificate}
function Expect-Failure([string]$Case,[string]$MessagePattern="") {$failed=$false;$message="";try{$null=Install-DriverPackage}catch{$failed=$true;$message=$_.Exception.Message};Assert-Check $failed "Expected safe failure: $Case";if($MessagePattern){Assert-Check ($message -match $MessagePattern) "Wrong failure reason for ${Case}: $message"}}
function Assert-NoChanges([string]$Case) {Assert-Check ($script:imports.Count -eq 0 -and $script:stagingCalls -eq 0 -and $script:setupCalls -eq 0) "No mutations expected: $Case"}
foreach($version in @('1.2.0.10','1.2.0.11')) {
 Reset-Case;$script:installed=$version;$script:AllowLocalGamepadCertificateTrust=1;$script:ConsentCertificateThumbprint=$script:certificate.Thumbprint
 Assert-Check ((Install-DriverPackage) -eq 0) 'Equal/newer must skip';Assert-NoChanges 'equal/newer';Assert-Check ($script:validationCalls -eq 0 -and $script:storeReads -eq 0) 'Equal/newer must bypass package and trust reads'
}
Reset-Case;$script:installed='unknown';Expect-Failure 'unknown active version';Assert-NoChanges 'unknown';Assert-Check ($script:storeReads -eq 0) 'Unknown must bypass trust'
Reset-Case;$script:queryFailure=$true;Expect-Failure 'inventory error';Assert-NoChanges 'query error'
foreach($status in @('NotTrusted','UnknownError')) {
 Reset-Case;$script:untrustedStatus=$status;$script:AllowLocalGamepadCertificateTrust=1;$script:ConsentCertificateThumbprint=$script:certificate.Thumbprint
 Assert-Check ((Install-DriverPackage) -eq 0) "Independently validated $status must reach exact consent and post-trust validation"
 Assert-Check ($script:validationCalls -eq 1 -and $script:stagingCalls -eq 1) 'Independent validation required before staging'
 Reset-Case;$script:untrustedStatus=$status;$script:validationFailure=$true;$script:AllowLocalGamepadCertificateTrust=1;$script:ConsentCertificateThumbprint=$script:certificate.Thumbprint
 Expect-Failure "Invalid package with $status";Assert-NoChanges "Invalid $status";Assert-Check ($script:storeReads -eq 0) 'Invalid package cannot reach trust'
}
foreach($status in @('HashMismatch','NotSigned','NotSupported','Incompatible')) {
 Reset-Case;$script:untrustedStatus=$status;$script:AllowLocalGamepadCertificateTrust=1;$script:ConsentCertificateThumbprint=$script:certificate.Thumbprint
 Expect-Failure "Disallowed signature status $status";Assert-NoChanges $status
 Assert-Check ($script:validationCalls -eq 0) 'Disallowed signatures must fail at the early guard'
}
foreach($version in @($null,'1.2.0.9')) {
 Reset-Case;$script:installed=$version;Expect-Failure 'missing/older without consent';Assert-NoChanges 'no consent'
 Reset-Case;$script:installed=$version;$script:AllowLocalGamepadCertificateTrust=1;$script:ConsentCertificateThumbprint=('CD'*20);Expect-Failure 'wrong consent identity';Assert-NoChanges 'wrong consent'
 Reset-Case;$script:installed=$version;$script:AllowLocalGamepadCertificateTrust=1;$script:ConsentCertificateThumbprint=$script:certificate.Thumbprint
 Assert-Check ((Install-DriverPackage) -eq 0) 'Consented missing/older must install';Assert-Check ($script:imports.Count -eq 2 -and $script:stagingCalls -eq 1 -and $script:setupCalls -eq 1) 'Only expected consented stores/driver calls'
}
Reset-Case;$script:certificate.Thumbprint=('AB'*20);$script:AllowLocalGamepadCertificateTrust=1;$script:ConsentCertificateThumbprint=$script:certificate.Thumbprint;Expect-Failure 'untrusted alternate signer';Assert-NoChanges 'untrusted alternate signer'
Reset-Case;$script:stores.Root=@($script:certificate.Thumbprint);$script:AllowLocalGamepadCertificateTrust=1;$script:ConsentCertificateThumbprint=$script:certificate.Thumbprint;Expect-Failure 'partially trusted alternate signer';Assert-NoChanges 'partially trusted alternate signer'
Reset-Case;$script:stores.Root=@($script:certificate.Thumbprint);$script:stores.TrustedPublisher=@($script:certificate.Thumbprint);Assert-Check ((Install-DriverPackage) -eq 0) 'Already trusted alternate signer must install';Assert-Check ($script:imports.Count -eq 0 -and $script:stagingCalls -eq 1) 'Alternate signer must not change trust';$script:certificate.Thumbprint='7F1868BF86698A8674F40E686B02C8C702BD3B88'
Reset-Case;$script:validationFailure=$true;$script:AllowLocalGamepadCertificateTrust=1;$script:ConsentCertificateThumbprint=$script:certificate.Thumbprint;Expect-Failure 'payload/crypto tamper';Assert-NoChanges 'validation failure';Assert-Check ($script:storeReads -eq 0) 'Validation must precede consent/trust lookup'
Reset-Case;$script:AllowLocalGamepadCertificateTrust=1;$script:ConsentCertificateThumbprint=$script:certificate.Thumbprint;$script:stagingFailure=$true;Expect-Failure 'staging failure';Assert-Check ($script:removals.Count -eq 2 -and $script:stores.Root.Count -eq 0 -and $script:stores.TrustedPublisher.Count -eq 0 -and $script:setupCalls -eq 0) 'Staging failure must roll back only new trust before setup'
Reset-Case;$script:stores.Root=@($script:certificate.Thumbprint,'PREEXISTING');$script:stores.TrustedPublisher=@('OTHER');$script:AllowLocalGamepadCertificateTrust=1;$script:ConsentCertificateThumbprint=$script:certificate.Thumbprint;$script:stagingFailure=$true;Expect-Failure 'preexisting root plus staging failure';Assert-Check ($script:removals.Count -eq 1 -and $script:removals[0] -ceq 'TrustedPublisher' -and $script:stores.Root.Count -eq 2 -and $script:stores.TrustedPublisher[0] -ceq 'OTHER') 'Rollback must preserve all preexisting and unrelated certificates'
Reset-Case;$script:AllowLocalGamepadCertificateTrust=1;$script:ConsentCertificateThumbprint=$script:certificate.Thumbprint;$script:postTrustFailure=$true;Expect-Failure 'post-trust signature failure';Assert-Check ($script:removals.Count -eq 2 -and $script:stagingCalls -eq 0) 'Post-trust failure must roll back before staging'
Reset-Case;$script:AllowLocalGamepadCertificateTrust=1;$script:ConsentCertificateThumbprint=$script:certificate.Thumbprint;$script:importFailure=$true;Expect-Failure 'second store failure';Assert-Check ($script:removals.Count -eq 1 -and $script:stores.Root.Count -eq 0 -and $script:stagingCalls -eq 0) 'Partial import must roll back successful additions'
Reset-Case;$script:AllowLocalGamepadCertificateTrust=1;$script:ConsentCertificateThumbprint=$script:certificate.Thumbprint;$script:setupFailure=$true;Expect-Failure 'device setup failure';Assert-Check ($script:removals.Count -eq 2 -and $script:setupCalls -eq 1) 'Setup failure must roll back only newly added trust'
Reset-Case;$script:stores.Root=@($script:certificate.Thumbprint);$script:stores.TrustedPublisher=@($script:certificate.Thumbprint);Assert-Check ((Install-DriverPackage) -eq 0) 'Existing exact trust needs no new consent';Assert-Check ($script:imports.Count -eq 0 -and $script:stagingCalls -eq 1) 'Existing exact trust must remain unchanged'
foreach($mapperExit in @(0,3010)) {
 Reset-Case;$script:stores.Root=@($script:certificate.Thumbprint);$script:stores.TrustedPublisher=@($script:certificate.Thumbprint);$script:mapperInf='mapper.inf';$script:mapperExit=$mapperExit
 Assert-Check ((Install-DriverPackage) -eq $mapperExit) 'Mapper restart request must survive successful parent readiness'
 Assert-Check (($script:events -join ',') -ceq 'mapper.inf,mock.inf,setup') 'Mapper staging must precede source package staging and root creation'
}
Reset-Case;$script:stores.Root=@($script:certificate.Thumbprint);$script:stores.TrustedPublisher=@($script:certificate.Thumbprint);$script:mapperInf='mapper.inf';$script:stagingFailure=$true
Expect-Failure 'both mapper and base staging fail';Assert-Check ($script:stagingCalls -eq 2 -and $script:setupCalls -eq 0 -and ($script:events -join ',') -ceq 'mapper.inf,mock.inf') 'Mapper failure must still attempt the independently validated base package'
foreach($parentVersion in @('1.2.0.10')) {
 Reset-Case;$script:installed=$parentVersion;$script:mapperInf='mapper.inf';$script:stores.Root=@($script:certificate.Thumbprint);$script:stores.TrustedPublisher=@($script:certificate.Thumbprint)
 Assert-Check ((Install-DriverPackage) -eq 0) 'An existing parent must still repair a missing mapper'
 Assert-Check ($script:validationCalls -eq 1 -and ($script:events -join ',') -ceq 'mapper.inf' -and $script:setupCalls -eq 0) 'Mapper validation/staging must run independently without replacing the equal active parent'
 Reset-Case;$script:installed=$parentVersion;$script:mapperDeclared=$true
 Assert-Check ((Install-DriverPackage) -eq 0) 'Missing optional mapper must preserve the existing base driver';Assert-NoChanges 'missing mapper despite existing parent'
}
Reset-Case;$script:installed='1.2.0.9';$script:mapperDeclared=$true;$script:stores.Root=@($script:certificate.Thumbprint);$script:stores.TrustedPublisher=@($script:certificate.Thumbprint)
Assert-Check ((Install-DriverPackage) -eq 0 -and ($script:events -join ',') -ceq 'mock.inf,setup') 'Missing optional mapper must not prevent a valid base upgrade'
Reset-Case;$script:installed='1.2.0.9';$script:mapperInf='mapper.inf';$script:mapperOnlyFailure=$true;$script:stores.Root=@($script:certificate.Thumbprint);$script:stores.TrustedPublisher=@($script:certificate.Thumbprint)
Assert-Check ((Install-DriverPackage) -eq 0 -and ($script:events -join ',') -ceq 'mapper.inf,mock.inf,setup') 'Mapper staging failure must not prevent a valid base upgrade'
Reset-Case;$script:installed='1.2.0.10';$script:mapperInf='mapper.inf';$script:mapperStoreVersion='1.2.0.10';$script:stores.Root=@($script:certificate.Thumbprint);$script:stores.TrustedPublisher=@($script:certificate.Thumbprint);$script:stagingFailure=$true
Assert-Check ((Install-DriverPackage) -eq 0) 'Mapper binding failure must preserve the equal active base';$script:stagingFailure=$false
Assert-Check ((Install-DriverPackage) -eq 0) 'Retry must repair equal-version stored mapper with missing binding'
Assert-Check (($script:events -join ',') -ceq 'mapper.inf,mapper.inf' -and $script:setupCalls -eq 0) 'Retry must stage mapper again and preserve the equal active parent'
foreach($where in @('active','store')) {
 Reset-Case;$script:installed='1.2.0.10';$script:mapperInf='mapper.inf'
 if($where -ceq 'active'){$script:mapperInstalledVersion='1.2.0.11'}else{$script:mapperStoreVersion='1.2.0.11'}
 Assert-Check ((Install-DriverPackage) -eq 0) "Newer $where mapper must be isolated";Assert-NoChanges 'newer mapper must never be downgraded'
}
Reset-Case;$script:installed='1.2.0.10';$script:mapperInf='mapper.inf';$script:mapperVendorId=1
Assert-Check ((Install-DriverPackage) -eq 0) 'Mapper identity mismatch must preserve the equal base';Assert-NoChanges 'incompatible mapper payload'
# A package in Driver Store must not be confused with an existing root node.
Reset-Case;$script:parentStoreVersion='1.2.0.10';$script:mapperInf='mapper.inf';$script:stores.Root=@($script:certificate.Thumbprint);$script:stores.TrustedPublisher=@($script:certificate.Thumbprint)
Assert-Check ((Install-DriverPackage) -eq 0) 'Equal staged-only parent must create/verify its missing root'
Assert-Check (($script:events -join ',') -ceq 'mapper.inf,mock.inf,setup' -and $script:setupCalls -eq 1) 'Staged-only equal parent must reach source staging and setup after mapper'
foreach($where in @('active','store')) {
 Reset-Case;$script:mapperInf='mapper.inf'
 if($where -ceq 'active'){$script:installed='1.2.0.11'}else{$script:parentStoreVersion='1.2.0.11'}
 Expect-Failure "strictly newer $where parent" 'newer parent';Assert-NoChanges 'unknown future parent identity/transport'
 Assert-Check ($script:validationCalls -eq 0 -and $script:storeReads -eq 0) 'Newer parent must fail before package trust/staging changes'
}
Reset-Case;$script:parentStoreVersion='1.2.0.10'
Assert-Check ((Install-DriverPackage) -eq 0) 'Legacy staged-only guard behavior remains unchanged'
Assert-NoChanges 'legacy equal stored parent';Assert-Check ($script:validationCalls -eq 0) 'Legacy utility must retain its original skip policy'
$signature=[pscustomobject]@{SignerCertificate=$script:certificate}
$wrong=[pscustomobject]@{SignerCertificate=[pscustomobject]@{Thumbprint=('CD'*20)}}
$failed=$false;try{$null=Assert-LocalTestCertificateIdentity 'mock.cer' $signature $wrong}catch{$failed=$true};Assert-Check $failed 'Tool/CAT/certificate identity mismatch must fail before trust'
$raw=$ast.Extent.Text
$packageFunction=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Assert-DriverPackage'},$true).Extent.Text
Assert-Check ($packageFunction.IndexOf('Assert-ManifestPayloadContract') -lt $packageFunction.IndexOf('Assert-LocalGamepadCryptographicPackage')) 'Manifest hashes must precede independent crypto checks'
Assert-Check ($packageFunction.IndexOf('Assert-ReleaseLock') -lt $packageFunction.IndexOf('Assert-LocalGamepadCryptographicPackage')) 'Release lock must precede independent crypto checks'
$public=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Get-PublicLocalGamepadCertificate'},$true).Extent.Text
Assert-Check ($public.IndexOf('GetCertContentType') -lt $public.IndexOf('::new($bytes)')) 'Public certificate content type must be checked before constructor/key loading'
Assert-Check ($raw.Contains('Choice=2') -and $raw.Contains('CheckCatalogMember(catalog,path,tag')) 'Actual WinVerifyTrust catalog membership is required before consent'
Write-Output 'VHF exact-certificate consent tests passed: version/no-consent/identity/tamper/failure rollback/preserved trust; mapper independent repair/retry/downgrade guards; staged-only root repair and strictly newer parent rejection. All trust, inventory, staging and setup calls were mocked.'
