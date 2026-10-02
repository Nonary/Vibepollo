param([switch]$CompileBridge)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$bridge = Join-Path $repo 'plugins/playnite/SunshinePlaynite/src/WaveformLaunchBridge.cs'
Add-Type -Path $bridge
if ([SunshinePlaynite.WaveformLaunchBridge]::Prepare('', 'mapping', $repo, $false)) {
  throw 'Empty library path accepted'
}
'Compiled Playnite bridge compiles and rejects missing libraries'
