param(
  [Parameter(Mandatory = $true)]
  [string]$BuildDir,
  [Parameter(Mandatory = $true)]
  [string]$OutDir
)

$ErrorActionPreference = "Stop"

function Write-Step {
  param([string]$Message)
  Write-Host "[webrtc-cleanup] $Message"
}

function Get-NormalizedPath {
  param([string]$Path)

  return [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

function Test-PathWithin {
  param(
    [string]$Candidate,
    [string]$Container
  )

  $candidatePath = Get-NormalizedPath $Candidate
  $containerPath = Get-NormalizedPath $Container
  if ($candidatePath.Equals($containerPath, [System.StringComparison]::OrdinalIgnoreCase)) {
    return $true
  }

  $containerPrefix = $containerPath + [System.IO.Path]::DirectorySeparatorChar
  return $candidatePath.StartsWith($containerPrefix, [System.StringComparison]::OrdinalIgnoreCase)
}

function Require-NonemptyFile {
  param([string]$Path)

  $item = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
  if (-not $item -or $item.PSIsContainer -or $item.Length -le 0) {
    throw "Required WebRTC artifact is missing or empty: $Path"
  }
}

function Assert-NoReparsePointInPath {
  param(
    [string]$Path,
    [string]$Label
  )

  $current = Get-NormalizedPath $Path
  while ($current) {
    $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
    if ($item -and ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
      throw "Refusing WebRTC cleanup through a reparse point in ${Label}: $current"
    }

    $parent = Split-Path -Parent $current
    if (-not $parent -or $parent.Equals($current, [System.StringComparison]::OrdinalIgnoreCase)) {
      break
    }
    $current = $parent
  }
}

$buildRoot = Get-NormalizedPath $BuildDir
$outputRoot = Get-NormalizedPath $OutDir
$driveRoot = Get-NormalizedPath ([System.IO.Path]::GetPathRoot($buildRoot))

if ($buildRoot.Equals($driveRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
  throw "Refusing to clean a drive root: $buildRoot"
}
if (Test-PathWithin -Candidate $outputRoot -Container $buildRoot) {
  throw "WEBRTC_OUT_DIR must be outside WEBRTC_BUILD_DIR before source cleanup. BuildDir='$buildRoot', OutDir='$outputRoot'."
}
Assert-NoReparsePointInPath -Path $buildRoot -Label "WEBRTC_BUILD_DIR"
Assert-NoReparsePointInPath -Path $outputRoot -Label "WEBRTC_OUT_DIR"

$requiredOutputFiles = @(
  (Join-Path $outputRoot "include\libwebrtc.h"),
  (Join-Path $outputRoot "lib\libwebrtc.dll"),
  (Join-Path $outputRoot "lib\libwebrtc.dll.a")
)
foreach ($requiredFile in $requiredOutputFiles) {
  Require-NonemptyFile $requiredFile
}

if (-not (Test-Path -LiteralPath $buildRoot)) {
  Write-Step "WebRTC source workspace is already absent: $buildRoot"
  return
}

$requiredBuildFiles = @(
  (Join-Path $buildRoot ".gclient"),
  (Join-Path $buildRoot "src\BUILD.gn"),
  (Join-Path $buildRoot "src\DEPS"),
  (Join-Path $buildRoot "src\out\mingw\libwebrtc.dll")
)
foreach ($requiredFile in $requiredBuildFiles) {
  Require-NonemptyFile $requiredFile
}

$builtDll = Join-Path $buildRoot "src\out\mingw\libwebrtc.dll"
$stagedDll = Join-Path $outputRoot "lib\libwebrtc.dll"
$builtDllHash = (Get-FileHash -LiteralPath $builtDll -Algorithm SHA256).Hash
$stagedDllHash = (Get-FileHash -LiteralPath $stagedDll -Algorithm SHA256).Hash
if ($builtDllHash -ne $stagedDllHash) {
  throw "Staged libwebrtc.dll does not match the completed build. Refusing to remove '$buildRoot'."
}

$currentPath = Get-NormalizedPath (Get-Location).Path
if (Test-PathWithin -Candidate $currentPath -Container $buildRoot) {
  Set-Location (Split-Path -Parent $buildRoot)
}

Write-Step "Removing disposable WebRTC source workspace: $buildRoot"
Remove-Item -LiteralPath $buildRoot -Recurse -Force
if (Test-Path -LiteralPath $buildRoot) {
  throw "WebRTC source workspace still exists after cleanup: $buildRoot"
}

Write-Step "Preserved reusable WebRTC SDK: $outputRoot"
