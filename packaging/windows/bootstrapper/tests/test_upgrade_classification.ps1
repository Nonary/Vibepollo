$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$framework=Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319'
$output=Join-Path ([IO.Path]::GetTempPath()) ('setup-order-'+[guid]::NewGuid().ToString('N')+'.exe')
$compiler=@('/nologo','/target:winexe',"/out:$output")
foreach($reference in @('System.dll','System.Core.dll','System.Data.dll','System.Xml.dll','System.Xaml.dll','System.Windows.Forms.dll','WPF/WindowsBase.dll','WPF/PresentationCore.dll','WPF/PresentationFramework.dll')){$compiler+='/reference:'+(Join-Path $framework $reference)}
$compiler+=(Join-Path $repo 'packaging/windows/bootstrapper/VibeshineInstaller.cs')
& (Join-Path $framework 'csc.exe') @compiler
if($LASTEXITCODE -ne 0){throw 'Setup classification compilation failed'}
try {
 $assembly=[Reflection.Assembly]::Load([IO.File]::ReadAllBytes($output))
 $flags=[Reflection.BindingFlags]'Instance,NonPublic'
 $window=@($assembly.GetTypes() | Where-Object {$_.GetMethod('GetInstallActionKind',$flags)})[0]
 $installedType=$window.GetField('_installedProduct',$flags).FieldType
 $payloadType=$window.GetField('_payloadMsiInfo',$flags).FieldType
 function Check-Order([string]$installed,[string]$target,[string]$expected,[string]$oldCode,[string]$newCode) {
  # No constructor, entry point, registry mutation or MSI execution.
  $instance=[Runtime.Serialization.FormatterServices]::GetUninitializedObject($window)
  $old=[Activator]::CreateInstance($installedType,$true);$old.Version=[version]$installed;$old.ProductCode=$oldCode
  $new=[Activator]::CreateInstance($payloadType,$true);$new.Version=[version]$target;$new.ProductCode=$newCode
  $window.GetField('_installedProduct',$flags).SetValue($instance,$old)
  $window.GetField('_payloadMsiInfo',$flags).SetValue($instance,$new)
  $actual=$window.GetMethod('GetInstallActionKind',$flags).Invoke($instance,@()).ToString()
  if($actual -ne $expected){throw "Setup $installed -> $target expected $expected got $actual"}
 }
 Check-Order '2.0.99.0' '2.0.90.0' 'Downgrade' '' ''
 Check-Order '2.0.99.0' '2.0.190.0' 'Upgrade' '' ''
 Check-Order '2.0.90.0' '2.0.190.0' 'Upgrade' '' ''
 Check-Order '2.0.190.0' '2.0.199.0' 'Upgrade' '' ''
 Check-Order '2.0.199.0' '2.0.190.0' 'Downgrade' '' ''
 Check-Order '2.0.190.0' '2.0.190.0' 'Reinstall' '' ''
 # Known UUIDv7 presentation hints; these do not establish MSI ordering.
 $older='{01A12B94-FB65-7E1E-9604-6BDE51F787D6}';$newer='{01A12B98-4F2E-7315-9F87-EFAA9540F09A}'
 Check-Order '2.0.190.0' '2.0.190.0' 'Upgrade' $older $newer
 Check-Order '2.0.190.0' '2.0.190.0' 'Downgrade' $newer $older
 Write-Output 'Compiled setup upgrade/downgrade/reinstall checks passed'
} finally {Remove-Item -LiteralPath $output -ErrorAction SilentlyContinue}
