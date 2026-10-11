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
[pscustomobject]@{
 verified=$true;sha256=$embedded
 setup_file_version=[Diagnostics.FileVersionInfo]::GetVersionInfo((Resolve-Path -LiteralPath $SetupPath).Path).FileVersion
 msi_product_version=Read-MsiProperty 'ProductVersion'
 msi_product_code=Read-MsiProperty 'ProductCode'
 msi_upgrade_code=Read-MsiProperty 'UpgradeCode'
} | ConvertTo-Json -Compress
