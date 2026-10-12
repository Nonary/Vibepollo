param([Parameter(Mandatory=$true)][string]$MsiPath)
$ErrorActionPreference='Stop'
$installer=New-Object -ComObject WindowsInstaller.Installer
$db=$installer.OpenDatabase([IO.Path]::GetFullPath($MsiPath),0)
function Rows($Query,$Names) {
 $view=$db.OpenView($Query)
 try { $null=$view.Execute(); while($record=$view.Fetch()) { $row=@{};for($i=0;$i -lt $Names.Count;$i++){$row[$Names[$i]]=$record.StringData($i+1)};[pscustomobject]$row } }
 finally { $null=$view.Close() }
}
$actions=@(Rows 'SELECT `Action`,`Source`,`Target` FROM `CustomAction`' @('name','property','value'))
$sequence=@(Rows 'SELECT `Action`,`Condition`,`Sequence` FROM `InstallExecuteSequence`' @('name','condition','step'))
$names=@('RestoreSavedDisplayDriverSelection','DefaultDisplayDriverSelection','RestoreSavedGamepadDriverSelection','DefaultGamepadDriverSelection','EnsureDriverSelectionsRepair','ExtendDriverSelectionsRepair','DefaultDriverSelectionsRepairMode')
$tested=@($sequence|Where-Object{$_.name -in $names}|Sort-Object {[int]$_.step})
if($tested.Count -ne 7){throw 'Missing MSI driver preference actions'}
$cost=[int]($sequence|Where-Object{$_.name -eq 'CostInitialize'}).step
if(@($tested|Where-Object{[int]$_.step -ge $cost}).Count -ne 0){throw 'Repair choices must precede component costing'}
$components=@(Rows 'SELECT `Feature_`,`Component_` FROM `FeatureComponents`' @('feature','component'))
$prefs=@($components|Where-Object{$_.feature -eq 'InstallerDriverSelections'})
if($prefs.Count -ne 1 -or $prefs[0].component -ne 'InstallerDriverSelectionPreferences'){throw 'Preference maintenance is not isolated'}
$keys=@(Rows 'SELECT `Root`,`Key`,`Name`,`Value`,`Component_` FROM `Registry`' @('root','key','name','value','component')|Where-Object{$_.component -eq 'InstallerDriverSelectionPreferences'})
if($keys.Count -ne 2 -or @($keys|Where-Object{$_.root -ne '2'}).Count -ne 0){throw 'Preferences must contain only two HKLM values'}
# Open only a metadata session. Never call DoAction, InstallProduct or configure a feature.
$session=$installer.OpenPackage([IO.Path]::GetFullPath($MsiPath),1)
function Set-Property($Name,$Value){$null=$session.GetType().InvokeMember('Property',[Reflection.BindingFlags]::SetProperty,$null,$session,@($Name,[string]$Value))}
function Get-Property($Name){return $session.GetType().InvokeMember('Property',[Reflection.BindingFlags]::GetProperty,$null,$session,@($Name))}
$cleared=@('Installed','REMOVE','REINSTALL','REINSTALLMODE','INSTALL_DISPLAY_DRIVERS','INSTALL_VIRTUAL_GAMEPAD_DRIVER','SAVED_DISPLAY_DRIVER_SELECTION','SAVED_GAMEPAD_DRIVER_SELECTION')
$cases=@(
 @{name='fresh';input=@{};display='1';gamepad='1';repair='';mode=''},
 @{name='old missing preferences';input=@{Installed='1'};display='1';gamepad='1';repair='InstallerDriverSelections';mode='m'},
 @{name='persisted optouts';input=@{Installed='1';SAVED_DISPLAY_DRIVER_SELECTION='0';SAVED_GAMEPAD_DRIVER_SELECTION='0'};display='0';gamepad='0';repair='InstallerDriverSelections';mode='m'},
 @{name='caller overrides optouts';input=@{Installed='1';INSTALL_DISPLAY_DRIVERS='1';INSTALL_VIRTUAL_GAMEPAD_DRIVER='1';SAVED_DISPLAY_DRIVER_SELECTION='0';SAVED_GAMEPAD_DRIVER_SELECTION='0'};display='1';gamepad='1';repair='InstallerDriverSelections';mode='m'},
 @{name='caller overrides enabled';input=@{Installed='1';INSTALL_DISPLAY_DRIVERS='0';INSTALL_VIRTUAL_GAMEPAD_DRIVER='0';SAVED_DISPLAY_DRIVER_SELECTION='1';SAVED_GAMEPAD_DRIVER_SELECTION='1'};display='0';gamepad='0';repair='InstallerDriverSelections';mode='m'},
 @{name='malformed saved values';input=@{Installed='1';SAVED_DISPLAY_DRIVER_SELECTION='invalid';SAVED_GAMEPAD_DRIVER_SELECTION='#0'};display='1';gamepad='1';repair='InstallerDriverSelections';mode='m'},
 @{name='Apollo full repair';input=@{Installed='1';REINSTALL='ALL';REINSTALLMODE='vams'};display='1';gamepad='1';repair='ALL';mode='vams'},
 @{name='targeted repair';input=@{Installed='1';REINSTALL='ProductFeature'};display='1';gamepad='1';repair='ProductFeature,InstallerDriverSelections';mode='m'},
 @{name='targeted explicit mode';input=@{Installed='1';REINSTALL='ProductFeature';REINSTALLMODE='omus'};display='1';gamepad='1';repair='ProductFeature,InstallerDriverSelections';mode='omus'},
 @{name='preference repair already present';input=@{Installed='1';REINSTALL='ProductFeature,InstallerDriverSelections';REINSTALLMODE='m'};display='1';gamepad='1';repair='ProductFeature,InstallerDriverSelections';mode='m'},
 @{name='uninstall';input=@{Installed='1';REMOVE='ALL'};display='1';gamepad='1';repair='';mode=''}
)
foreach($case in $cases){
 foreach($name in $cleared){Set-Property $name ''}
 foreach($name in $case.input.Keys){Set-Property $name $case.input[$name]}
 foreach($step in $tested){
  $evaluation=$session.EvaluateCondition($step.condition)
  if($evaluation -eq 3){throw "Invalid native MSI condition: $($step.name)"}
  if($evaluation -eq 1){
   $action=$actions|Where-Object{$_.name -eq $step.name}
   $record=$installer.CreateRecord(0)
   $null=$record.GetType().InvokeMember('StringData',[Reflection.BindingFlags]::SetProperty,$null,$record,@(0,$action.value))
   Set-Property $action.property ($session.FormatRecord($record))
  }
 }
 $expected=@{INSTALL_DISPLAY_DRIVERS=$case.display;INSTALL_VIRTUAL_GAMEPAD_DRIVER=$case.gamepad;REINSTALL=$case.repair;REINSTALLMODE=$case.mode}
 foreach($name in $expected.Keys){if((Get-Property $name) -cne $expected[$name]){throw "$($case.name): $name expected '$($expected[$name])', got '$(Get-Property $name)'"}}
}
Write-Host '11 native MSI condition/property-format cases, registry-only feature and pre-costing scheduling passed. No MSI actions, registry writes or installations executed.'
