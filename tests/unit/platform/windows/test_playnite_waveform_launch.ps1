param([switch]$CompileBridge, [string]$NativeHelperDirectory)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$bridge = Join-Path $repo 'plugins/playnite/SunshinePlaynite/src/WaveformLaunchBridge.cs'
Add-Type -Path $bridge
if ([SunshinePlaynite.WaveformLaunchBridge]::Prepare('', 'mapping', $repo, $false)) {
  throw 'Empty library path accepted'
}
if ($NativeHelperDirectory) {
  # Steam preparation must reach the 64-bit helper even from 32-bit Playnite.
  $missingMapping = 'Local\Vibepollo.Missing.Mapping.' + [Guid]::NewGuid().ToString('N')
  $dll = Join-Path $NativeHelperDirectory 'vibepollo_dualsense_audio.dll'
  $failure = $null
  try { [void][SunshinePlaynite.WaveformLaunchBridge]::Prepare($dll, $missingMapping, $repo, $true) }
  catch { $failure = $_.Exception.ToString() }
  if (-not $failure -or $failure -notmatch 'Steam hook preparation failed') {
    throw "Storefront setup did not reach the native helper: $failure"
  }
  "Native helper reached from $([IntPtr]::Size * 8)-bit connector host"
}
'Compiled Playnite bridge rejects missing libraries; routing is tested by the connector lifecycle harness'
