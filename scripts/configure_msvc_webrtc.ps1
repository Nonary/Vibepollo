param(
  [string]$SourceDir = "",
  [string]$BuildDir = "",
  [string]$WebrtcRoot = "",
  [string]$BuildType = "",
  [string]$VcVarsPath = "",
  [string]$CMakeExe = "",
  [string]$CMakeCache = ""
)

$ErrorActionPreference = "Stop"

function Get-CacheValue {
  param(
    [string]$Path,
    [string]$Key
  )
  if (-not $Path -or -not (Test-Path $Path)) {
    return $null
  }
  $pattern = "^$([regex]::Escape($Key)):[^=]*=(.*)$"
  foreach ($line in Get-Content -Path $Path) {
    if ($line -match $pattern) {
      return $matches[1]
    }
  }
  return $null
}

if (-not $CMakeCache) {
  if ($env:CMAKE_CACHE_FILE) {
    $CMakeCache = $env:CMAKE_CACHE_FILE
  } elseif ($env:SUNSHINE_CMAKE_CACHE) {
    $CMakeCache = $env:SUNSHINE_CMAKE_CACHE
  }
}

function Resolve-Value {
  param(
    [string]$Value,
    [string]$CacheKey,
    [string]$EnvKey
  )
  if ($Value) {
    return $Value
  }
  $cacheValue = Get-CacheValue -Path $CMakeCache -Key $CacheKey
  if ($cacheValue) {
    return $cacheValue
  }
  if ($EnvKey) {
    $envValue = [Environment]::GetEnvironmentVariable($EnvKey)
    if ($envValue) {
      return $envValue
    }
  }
  return ""
}

$scriptRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$SourceDir = Resolve-Value -Value $SourceDir -CacheKey "CMAKE_HOME_DIRECTORY" -EnvKey "SUNSHINE_ROOT_DIR"
if (-not $SourceDir) {
  $SourceDir = $scriptRoot
}
$SourceDir = (Resolve-Path $SourceDir).Path

$BuildDir = Resolve-Value -Value $BuildDir -CacheKey "WEBRTC_MSVC_BUILD_DIR" -EnvKey "WEBRTC_MSVC_BUILD_DIR"
if (-not $BuildDir) {
  $BuildDir = Join-Path $SourceDir "build-msvc"
}

$WebrtcRoot = Resolve-Value -Value $WebrtcRoot -CacheKey "WEBRTC_ROOT" -EnvKey "WEBRTC_ROOT"
if (-not $WebrtcRoot) {
  $WebrtcRoot = Join-Path $SourceDir "build\libwebrtc"
}

$BuildType = Resolve-Value -Value $BuildType -CacheKey "CMAKE_BUILD_TYPE" -EnvKey "CMAKE_BUILD_TYPE"
if (-not $BuildType) {
  $BuildType = "Debug"
}

$VcVarsPath = Resolve-Value -Value $VcVarsPath -CacheKey "WEBRTC_VCVARS_PATH" -EnvKey "WEBRTC_VCVARS_PATH"
if (-not $VcVarsPath -and $env:VSINSTALLDIR) {
  $candidate = Join-Path $env:VSINSTALLDIR "VC\Auxiliary\Build\vcvars64.bat"
  if (Test-Path $candidate) {
    $VcVarsPath = $candidate
  }
}

if (-not $VcVarsPath -or -not (Test-Path $VcVarsPath)) {
  throw "vcvars64.bat not found; set WEBRTC_VCVARS_PATH or VSINSTALLDIR."
}

if (-not (Test-Path $WebrtcRoot)) {
  throw "WEBRTC_ROOT not found: $WebrtcRoot"
}

$cmake = Resolve-Value -Value $CMakeExe -CacheKey "CMAKE_COMMAND" -EnvKey "CMAKE_COMMAND"
if (-not $cmake) {
  $cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
  if ($cmakeCmd) {
    $cmake = $cmakeCmd.Source
  }
}
if (-not $cmake -or -not (Test-Path $cmake)) {
  throw "cmake.exe not found; set CMAKE_COMMAND or CMakeExe."
}

if (-not (Test-Path $BuildDir)) {
  New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
}

$args = @(
  "`"$VcVarsPath`"",
  "&&",
  "`"$cmake`"",
  "-G", "Ninja",
  "-S", "`"$SourceDir`"",
  "-B", "`"$BuildDir`"",
  "-DCMAKE_C_COMPILER=cl",
  "-DCMAKE_CXX_COMPILER=cl",
  "-DSUNSHINE_ENABLE_WEBRTC=ON",
  "-DWEBRTC_ROOT=`"$WebrtcRoot`"",
  "-DWEBRTC_INCLUDE_DIR=`"$WebrtcRoot\include`"",
  "-DWEBRTC_LIBRARY=`"$WebrtcRoot\lib\libwebrtc.dll.lib`"",
  "-DCMAKE_BUILD_TYPE=$BuildType",
  "-DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON",
  "-DBUILD_TESTS=OFF",
  "-DBUILD_DOCS=OFF"
)

$cmd = "call " + ($args -join " ")
cmd.exe /C $cmd
