param(
  [string]$RootDir = "",
  [string]$BuildDir = "",
  [string]$OutDir = "",
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Release",
  [string]$WebrtcBranch = "",
  [string]$WebrtcRepoUrl = "",
  [string]$Msys2Bin = "",
  [string]$GitBin = "",
  [string]$GitExe = "",
  [string]$VisualStudioPath = "",
  [string]$WinSdkDir = "",
  [string]$ClangBasePath = "",
  [string]$DepotToolsDir = "",
  [string]$GitCachePath = "",
  [int]$GclientJobs = 0,
  [string]$CMakeCache = ""
)

$ErrorActionPreference = "Stop"

function Write-Step {
  param([string]$Message)
  Write-Host "[webrtc] $Message"
}

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
$RootDir = Resolve-Value -Value $RootDir -CacheKey "CMAKE_HOME_DIRECTORY" -EnvKey "SUNSHINE_ROOT_DIR"
if (-not $RootDir) {
  $RootDir = $scriptRoot
}
$RootDir = (Resolve-Path $RootDir).Path

$BuildDir = Resolve-Value -Value $BuildDir -CacheKey "WEBRTC_BUILD_DIR" -EnvKey "WEBRTC_BUILD_DIR"
if (-not $BuildDir) {
  $BuildDir = Join-Path $RootDir "build\libwebrtc-src"
}

$OutDir = Resolve-Value -Value $OutDir -CacheKey "WEBRTC_OUT_DIR" -EnvKey "WEBRTC_OUT_DIR"
if (-not $OutDir) {
  $OutDir = Join-Path $RootDir "build\libwebrtc"
}

$Configuration = Resolve-Value -Value $Configuration -CacheKey "WEBRTC_CONFIGURATION" -EnvKey "WEBRTC_CONFIGURATION"
if (-not $Configuration) {
  $Configuration = "Release"
}

$WebrtcBranch = Resolve-Value -Value $WebrtcBranch -CacheKey "WEBRTC_BRANCH" -EnvKey "WEBRTC_BRANCH"
if (-not $WebrtcBranch) {
  $WebrtcBranch = "m125_release"
}

$WebrtcRepoUrl = Resolve-Value -Value $WebrtcRepoUrl -CacheKey "WEBRTC_REPO_URL" -EnvKey "WEBRTC_REPO_URL"
if (-not $WebrtcRepoUrl) {
  $WebrtcRepoUrl = "https://github.com/webrtc-sdk/webrtc.git"
}

$Msys2Bin = Resolve-Value -Value $Msys2Bin -CacheKey "WEBRTC_MSYS2_BIN" -EnvKey "WEBRTC_MSYS2_BIN"
$GitExe = Resolve-Value -Value $GitExe -CacheKey "WEBRTC_GIT_EXE" -EnvKey "WEBRTC_GIT_EXE"
$GitBin = Resolve-Value -Value $GitBin -CacheKey "WEBRTC_GIT_BIN" -EnvKey "WEBRTC_GIT_BIN"
$VisualStudioPath = Resolve-Value -Value $VisualStudioPath -CacheKey "WEBRTC_VS_PATH" -EnvKey "WEBRTC_VS_PATH"
$WinSdkDir = Resolve-Value -Value $WinSdkDir -CacheKey "WEBRTC_WINSDK_DIR" -EnvKey "WEBRTC_WINSDK_DIR"
$ClangBasePath = Resolve-Value -Value $ClangBasePath -CacheKey "WEBRTC_CLANG_BASE_PATH" -EnvKey "WEBRTC_CLANG_BASE_PATH"
$DepotToolsDir = Resolve-Value -Value $DepotToolsDir -CacheKey "WEBRTC_DEPOT_TOOLS_DIR" -EnvKey "WEBRTC_DEPOT_TOOLS_DIR"
$GitCachePath = Resolve-Value -Value $GitCachePath -CacheKey "WEBRTC_GIT_CACHE_DIR" -EnvKey "WEBRTC_GIT_CACHE_DIR"
$GclientJobs = [int](Resolve-Value -Value ([string]$GclientJobs) -CacheKey "WEBRTC_GCLIENT_JOBS" -EnvKey "WEBRTC_GCLIENT_JOBS")

if (-not (Test-Path $RootDir)) {
  throw "RootDir not found: $RootDir"
}

if (-not (Test-Path $BuildDir)) {
  New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

$depotToolsDir = if ($DepotToolsDir) { $DepotToolsDir } else { Join-Path $BuildDir "depot_tools" }
if (-not (Test-Path $depotToolsDir)) {
  Write-Step "Cloning depot_tools"
  git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git $depotToolsDir
}

$gitShimDir = Join-Path $BuildDir "git-shim"
if (-not (Test-Path $gitShimDir)) {
  New-Item -ItemType Directory -Path $gitShimDir | Out-Null
}
$gitBat = Join-Path $gitShimDir "git.bat"
if (-not (Test-Path $gitBat)) {
  if (-not $GitExe) {
    $gitCommand = Get-Command git -ErrorAction SilentlyContinue
    if ($gitCommand) {
      $GitExe = $gitCommand.Source
    }
  }
  if (-not $GitExe -or -not (Test-Path $GitExe)) {
    throw "GitExe not found; set WEBRTC_GIT_EXE or ensure git is in PATH."
  }
  "@echo off`r`n`"$GitExe`" %*`r`n" | Set-Content -Path $gitBat -Encoding ASCII
}
if (-not $GitBin -and $GitExe) {
  $GitBin = Split-Path -Parent $GitExe
}

$pathParts = @($gitShimDir, $depotToolsDir, $GitBin) | Where-Object { $_ }
if ($Msys2Bin) {
  $pathParts += $Msys2Bin
}
$env:PATH = ($pathParts -join ";") + ";$env:PATH"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = "0"
$env:DEPOT_TOOLS_UPDATE = "0"
$env:GYP_MSVS_VERSION = "2022"
$env:GIT_CACHE_PATH = if ($GitCachePath) { $GitCachePath } else { (Join-Path $BuildDir "git-cache") }
if (-not (Test-Path $env:GIT_CACHE_PATH)) {
  New-Item -ItemType Directory -Path $env:GIT_CACHE_PATH | Out-Null
}
if ($VisualStudioPath) {
  $env:GYP_MSVS_OVERRIDE_PATH = $VisualStudioPath
  $env:vs2022_install = $VisualStudioPath
} elseif ($env:VSINSTALLDIR) {
  $env:GYP_MSVS_OVERRIDE_PATH = $env:VSINSTALLDIR
  $env:vs2022_install = $env:VSINSTALLDIR
}
if ($WinSdkDir) {
  $env:WINDOWSSDKDIR = $WinSdkDir
} elseif ($env:WindowsSdkDir) {
  $env:WINDOWSSDKDIR = $env:WindowsSdkDir
} elseif ($env:WINDOWSSDKDIR) {
  $env:WINDOWSSDKDIR = $env:WINDOWSSDKDIR
}

Set-Location $BuildDir

Write-Step "Initializing depot_tools"
Push-Location $depotToolsDir
cmd /c bootstrap\\win_tools.bat
cmd /c gclient
cmd /c gclient
Pop-Location

if (Test-Path (Join-Path $BuildDir "src\\build\\.git")) {
  Write-Step "Cleaning src\\build before sync"
  git -C (Join-Path $BuildDir "src\\build") reset --hard | Out-Null
  git -C (Join-Path $BuildDir "src\\build") clean -fdx | Out-Null
}

Write-Step "Writing .gclient"
@" 
solutions = [
  {
    "name"        : 'src',
    "url"         : '${WebrtcRepoUrl}@${WebrtcBranch}',
    "deps_file"   : 'DEPS',
    "managed"     : False,
    "custom_deps" : {
    },
    "custom_vars": {},
  },
]
target_os  = ['win']
"@ | Set-Content -Path (Join-Path $BuildDir ".gclient") -Encoding ASCII

Write-Step "Syncing WebRTC sources"
$syncJobs = if ($GclientJobs -gt 0) { $GclientJobs } else { 16 }
gclient sync --jobs $syncJobs
if ($LASTEXITCODE -ne 0) {
  throw "gclient sync failed"
}

$sdkRoot = if ($env:WINDOWSSDKDIR) { $env:WINDOWSSDKDIR } else { $WinSdkDir }
if (-not $sdkRoot) {
  throw "WinSdkDir not set; set WEBRTC_WINSDK_DIR or WindowsSdkDir environment."
}
$winSdkInclude = Join-Path $sdkRoot "Include"
if (-not (Test-Path $winSdkInclude)) {
  throw "WinSDK Include not found at $winSdkInclude"
}
$winSdkVersion = Get-ChildItem -Path $winSdkInclude -Directory |
  Sort-Object Name -Descending |
  Select-Object -First 1 -ExpandProperty Name
if (-not $winSdkVersion) {
  throw "Unable to detect Windows SDK version under $winSdkInclude"
}

$vsToolchainPath = Join-Path $BuildDir "src\\build\\vs_toolchain.py"
$setupToolchainPath = Join-Path $BuildDir "src\\build\\toolchain\\win\\setup_toolchain.py"
foreach ($path in @($vsToolchainPath, $setupToolchainPath)) {
  if (-not (Test-Path $path)) {
    throw "Toolchain file not found: $path"
  }
  $content = Get-Content $path -Raw
  $updated = $content -replace "SDK_VERSION = '([^']*)'", "SDK_VERSION = '$winSdkVersion'"
  if ($updated -ne $content) {
    Set-Content -Path $path -Value $updated -Encoding ASCII
  }
}

$localLibWebrtc = Join-Path $RootDir "third-party\libwebrtc"
$destLibWebrtc = Join-Path $BuildDir "src\libwebrtc"
if (-not (Test-Path $localLibWebrtc)) {
  throw "Local libwebrtc not found: $localLibWebrtc"
}
if (Test-Path $destLibWebrtc) {
  Remove-Item -Recurse -Force $destLibWebrtc
}
Write-Step "Copying libwebrtc wrapper into checkout"
Copy-Item -Recurse -Force $localLibWebrtc $destLibWebrtc

$buildGn = Join-Path $BuildDir "src\BUILD.gn"
$buildGnContent = Get-Content $buildGn -Raw
if ($buildGnContent -notmatch "//libwebrtc") {
  Write-Step "Patching BUILD.gn to include libwebrtc"
  $buildGnContent = $buildGnContent.Replace('deps = [ ":webrtc" ]', 'deps = [ ":webrtc", "//libwebrtc" ]')
  Set-Content -Path $buildGn -Value $buildGnContent -Encoding ASCII
}

$isDebug = $Configuration -eq "Debug"
$gnArgs = @(
  'target_os=\"win\"',
  'target_cpu=\"x64\"',
  "is_debug=$($isDebug.ToString().ToLower())",
  'is_component_build=false',
  'rtc_use_h264=true',
  'ffmpeg_branding=\"Chrome\"',
  'rtc_include_tests=false',
  'rtc_build_examples=false',
  'libwebrtc_desktop_capture=true',
  'is_clang=true',
  'clang_use_chrome_plugins=false',
  'use_custom_libcxx=true',
  'treat_warnings_as_errors=false'
)
if ($ClangBasePath) {
  $gnArgs += "clang_base_path=`"$ClangBasePath`""
}
$gnArgsString = $gnArgs -join " "

$gnOutDir = Join-Path $BuildDir "src\out\mingw"
Write-Step "Generating GN build files"
gn gen $gnOutDir --root="$($BuildDir)\src" --args="$gnArgsString"

Write-Step "Building libwebrtc"
ninja -C $gnOutDir libwebrtc

if (-not (Test-Path $OutDir)) {
  New-Item -ItemType Directory -Path $OutDir | Out-Null
}
if (-not (Test-Path (Join-Path $OutDir "include"))) {
  New-Item -ItemType Directory -Path (Join-Path $OutDir "include") | Out-Null
}
if (-not (Test-Path (Join-Path $OutDir "lib"))) {
  New-Item -ItemType Directory -Path (Join-Path $OutDir "lib") | Out-Null
}

Write-Step "Copying headers"
Copy-Item -Recurse -Force (Join-Path $destLibWebrtc "include\*") (Join-Path $OutDir "include")

$dll = Join-Path $gnOutDir "libwebrtc.dll"
$dllA = Join-Path $gnOutDir "libwebrtc.dll.a"
$dllLib = Join-Path $gnOutDir "libwebrtc.dll.lib"

if (-not (Test-Path $dll)) {
  throw "libwebrtc.dll not found at $dll"
}

Write-Step "Copying runtime DLL"
Copy-Item -Force $dll (Join-Path $OutDir "lib\libwebrtc.dll")

if (Test-Path $dllA) {
  Write-Step "Copying MinGW import library"
  Copy-Item -Force $dllA (Join-Path $OutDir "lib\libwebrtc.dll.a")
} elseif (Test-Path $dllLib) {
  Write-Step "Generating MinGW import library from MSVC .lib"
  $gendef = Get-Command gendef -ErrorAction SilentlyContinue
  $dlltool = Get-Command dlltool -ErrorAction SilentlyContinue
  if (-not $gendef -or -not $dlltool) {
    throw "gendef/dlltool not found in PATH; install MSYS2 binutils."
  }
  $defFile = Join-Path $OutDir "lib\libwebrtc.def"
  & $gendef.Source $dll
  Move-Item -Force (Join-Path (Get-Location) "libwebrtc.def") $defFile
  & $dlltool.Source -d $defFile -l (Join-Path $OutDir "lib\libwebrtc.dll.a") -D libwebrtc.dll
} else {
  throw "No import library found in $gnOutDir"
}

if (Test-Path $dllLib) {
  Write-Step "Copying MSVC import library"
  Copy-Item -Force $dllLib (Join-Path $OutDir "lib\libwebrtc.dll.lib")
}

Write-Step "Done. Set WEBRTC_ROOT to $OutDir"
