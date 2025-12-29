param(
  [string]$BuildDir = "",
  [string]$VcVarsPath = "",
  [string]$CMakeExe = "",
  [string]$BuildType = "",
  [int]$Jobs = 0,
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
$BuildDir = Resolve-Value -Value $BuildDir -CacheKey "WEBRTC_MSVC_BUILD_DIR" -EnvKey "WEBRTC_MSVC_BUILD_DIR"
if (-not $BuildDir) {
  $BuildDir = Join-Path $scriptRoot "build-msvc"
}

$VcVarsPath = Resolve-Value -Value $VcVarsPath -CacheKey "WEBRTC_VCVARS_PATH" -EnvKey "WEBRTC_VCVARS_PATH"
if (-not $VcVarsPath -and $env:VSINSTALLDIR) {
  $candidate = Join-Path $env:VSINSTALLDIR "VC\Auxiliary\Build\vcvars64.bat"
  if (Test-Path $candidate) {
    $VcVarsPath = $candidate
  }
}

$BuildType = Resolve-Value -Value $BuildType -CacheKey "CMAKE_BUILD_TYPE" -EnvKey "CMAKE_BUILD_TYPE"
if (-not $BuildType) {
  $BuildType = "Debug"
}

$Jobs = [int](Resolve-Value -Value ([string]$Jobs) -CacheKey "WEBRTC_BUILD_JOBS" -EnvKey "WEBRTC_BUILD_JOBS")
if ($Jobs -le 0) {
  $Jobs = 10
}

if (-not $VcVarsPath -or -not (Test-Path $VcVarsPath)) {
  throw "vcvars64.bat not found; set WEBRTC_VCVARS_PATH or VSINSTALLDIR."
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

$args = @(
  "`"$VcVarsPath`"",
  "&&",
  "`"$cmake`"",
  "--build", "`"$BuildDir`"",
  "--config", $BuildType,
  "--target", "all",
  "-j", "$Jobs"
)

$cmd = "call " + ($args -join " ")
cmd.exe /C $cmd
