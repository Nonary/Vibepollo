# Builds vibeshine_truehdr.dll (MSVC) from truehdr_shim.cpp against the NVIDIA RTX Video SDK.
# Vibeshine itself builds with MinGW and loads this DLL at runtime, so it must be built
# separately with MSVC (the RTX Video SDK ships MSVC libraries). Run once after obtaining
# the SDK; re-run only if the shim or SDK changes.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File build_truehdr_shim.ps1 `
#       [-SdkPath <RTX_Video_SDK dir>] [-OutDir <output dir>] [-Vcvars <vcvars64.bat>]
#
# Output: <OutDir>\vibeshine_truehdr.dll plus the required TrueHDR NGX runtime DLL
#         (nvngx_truehdr.dll). The VSR runtime is intentionally not copied.
param(
    [string]$SdkPath = $env:NV_RTX_VIDEO_SDK,
    [string]$OutDir  = "$PSScriptRoot",
    [string]$Vcvars  = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrEmpty($SdkPath)) {
    # Fall back to a conventional location if the env var is unset.
    foreach ($c in @("D:\sources\RTX_Video_SDK", "C:\RTX_Video_SDK")) {
        if (Test-Path "$c\include\nvsdk_ngx.h") { $SdkPath = $c; break }
    }
}
if ([string]::IsNullOrEmpty($SdkPath) -or -not (Test-Path "$SdkPath\include\nvsdk_ngx.h")) {
    throw "RTX Video SDK not found. Set NV_RTX_VIDEO_SDK or pass -SdkPath (needs include\nvsdk_ngx.h)."
}

# Locate vcvars64.bat.
if ([string]::IsNullOrEmpty($Vcvars)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath
        if ($vs) { $Vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat" }
    }
    foreach ($c in @("D:\Software\Visual Studio\VC\Auxiliary\Build\vcvars64.bat")) {
        if ([string]::IsNullOrEmpty($Vcvars) -or -not (Test-Path $Vcvars)) { if (Test-Path $c) { $Vcvars = $c } }
    }
}
if (-not (Test-Path $Vcvars)) { throw "vcvars64.bat not found. Pass -Vcvars <path>." }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$inc = "$SdkPath\include"
$lib = "$SdkPath\lib\Windows\x64\nvsdk_ngx_d.lib"
$src = "$PSScriptRoot\truehdr_shim.cpp"
$dll = "$OutDir\vibeshine_truehdr.dll"

# Build the DLL. /MD so it matches the NGX dynamic import lib (nvsdk_ngx_d.lib).
$cl = "cl /nologo /EHsc /MD /O2 /std:c++17 /LD `"$src`" /I `"$inc`" /Fe:`"$dll`" /link `"$lib`" advapi32.lib user32.lib"
$bat = Join-Path $env:TEMP "build_truehdr_shim_$PID.bat"
@"
@echo off
call "$Vcvars" >nul
cd /d "$OutDir"
$cl
"@ | Set-Content -Encoding ascii $bat
& cmd /c $bat
Remove-Item $bat -ErrorAction SilentlyContinue

if (-not (Test-Path $dll)) { throw "Build failed: $dll not produced." }

# Copy the TrueHDR NGX feature runtime next to the shim. VSR is a separate feature
# and is not needed for RTX HDR.
foreach ($d in @("nvngx_truehdr.dll")) {
    $p = "$SdkPath\bin\Windows\x64\rel\$d"
    if (Test-Path $p) { Copy-Item $p $OutDir -Force }
}

Write-Output "OK: $dll"
Get-ChildItem $OutDir -Filter *.dll | ForEach-Object { Write-Output ("  {0}  {1} bytes" -f $_.Name, $_.Length) }
