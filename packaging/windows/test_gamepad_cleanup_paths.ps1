param([string]$RepositoryRoot=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path)
$ErrorActionPreference='Stop'
$tokens=$null;$errors=$null
$path=Join-Path $RepositoryRoot 'src_assets/windows/drivers/vhf-gamepad/cleanup.ps1'
$ast=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Cleanup parse failed'}
foreach($name in @('Resolve-SystemToolPath','Remove-OwnedRootDevices','Remove-OwnedDriverStorePackages')){
 $node=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name}.GetNewClosure(),$true)
 . ([scriptblock]::Create($node.Extent.Text))
}
# Empty inventories guarantee no native process/device removal can execute.
function Get-OwnedRootDeviceInstanceIds{return @()}
function Get-WindowsDriver{param([switch]$Online,[switch]$All)return @()}
function Get-Command{param($Name,$ErrorAction)return [pscustomobject]@{Name=$Name}}
function Write-DriverMessage{param($Message)}
function Test-Path{param($LiteralPath,$PathType) $script:probed+=@($LiteralPath);return $LiteralPath -eq $script:available}
foreach($architecture in @('Sysnative','System32','SysWOW64','missing')){
 $script:available=if($architecture -eq 'missing'){''}else{Join-Path $env:SystemRoot "$architecture/pnputil.exe"}
 foreach($operation in @('Remove-OwnedRootDevices','Remove-OwnedDriverStorePackages')){
  $script:probed=@();$failed=$false
  try{$null=& $operation}catch{$failed=$true}
  if($failed -ne ($architecture -eq 'missing')){throw "Path resolution mismatch: $architecture/$operation"}
  if($architecture -ne 'missing' -and $script:probed[-1] -ne $script:available){throw 'Native system tool path not used'}
 }
}
Write-Host 'Eight cleanup path/empty-inventory cases passed; no PnPUtil or driver operation executed.'
