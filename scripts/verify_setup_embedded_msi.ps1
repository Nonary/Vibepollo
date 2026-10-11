param(
 [Parameter(Mandatory=$true)][string]$SetupPath,
 [Parameter(Mandatory=$true)][string]$MsiPath
)
$ErrorActionPreference='Stop'
# Loading metadata/resources never invokes the setup entry point or MSI actions.
$assembly=[Reflection.Assembly]::ReflectionOnlyLoadFrom((Resolve-Path -LiteralPath $SetupPath).Path)
$stream=$assembly.GetManifestResourceStream('Payload.msi')
if($null -eq $stream){throw 'Setup has no embedded Payload.msi'}
$hasher=[Security.Cryptography.SHA256]::Create()
try {
 $embedded=([BitConverter]::ToString($hasher.ComputeHash($stream))).Replace('-','').ToLowerInvariant()
} finally {$stream.Dispose();$hasher.Dispose()}
$expected=(Get-FileHash -LiteralPath $MsiPath -Algorithm SHA256).Hash.ToLowerInvariant()
if($embedded -cne $expected){throw 'Setup embedded MSI differs from the exact qualified internal MSI'}
$installer=New-Object -ComObject WindowsInstaller.Installer
$database=$installer.OpenDatabase((Resolve-Path -LiteralPath $MsiPath).Path,0)
function Read-MsiProperty([string]$Name) {
 $view=$database.OpenView("SELECT ``Value`` FROM ``Property`` WHERE ``Property``='$Name'")
 try{$null=$view.Execute();$record=$view.Fetch();if($null -eq $record){throw "Missing MSI property $Name"};return $record.StringData(1)}finally{$null=$view.Close()}
}
# ProductCode ordering is a setup hint; MSI ordering/replacement comes from this table.
$upgradeCode=Read-MsiProperty 'UpgradeCode'
if((Read-MsiProperty 'VIBESHINE_TRANSACTIONAL_REPLACEMENT') -ne '1'){throw 'MSI lacks transactional replacement contract'}
$view=$database.OpenView('SELECT `UpgradeCode`,`VersionMin`,`VersionMax`,`Attributes`,`ActionProperty` FROM `Upgrade`')
$replacement=$false
try {
 $null=$view.Execute()
 while($row=$view.Fetch()) {
  if($row.StringData(1) -eq $upgradeCode -and $row.StringData(2) -eq '0' -and
     $row.StringData(3) -eq '' -and ([int]$row.IntegerData(4) -band 256) -ne 0 -and
     ([int]$row.IntegerData(4) -band 2) -eq 0 -and $row.StringData(5) -eq 'WIX_UPGRADE_DETECTED'){$replacement=$true}
 }
} finally {$null=$view.Close()}
if(!$replacement){throw 'MSI Upgrade table does not include same-version replacements'}
$sequence=@{}
$view=$database.OpenView('SELECT `Action`,`Sequence` FROM `InstallExecuteSequence`')
try{$null=$view.Execute();while($row=$view.Fetch()){$sequence[$row.StringData(1)]=[int]$row.IntegerData(2)}}finally{$null=$view.Close()}
if(!$sequence.ContainsKey('RemoveExistingProducts') -or
   $sequence['RemoveExistingProducts'] -le $sequence['InstallInitialize'] -or
   $sequence['RemoveExistingProducts'] -ge $sequence['InstallFinalize']){throw 'MSI replacement must run within the rollback transaction'}
[pscustomobject]@{
 verified=$true;sha256=$embedded;upgrade_table_verified=$true
 setup_file_version=[Diagnostics.FileVersionInfo]::GetVersionInfo((Resolve-Path -LiteralPath $SetupPath).Path).FileVersion
 msi_product_version=Read-MsiProperty 'ProductVersion'
 msi_product_code=Read-MsiProperty 'ProductCode'
 msi_upgrade_code=Read-MsiProperty 'UpgradeCode'
} | ConvertTo-Json -Compress
