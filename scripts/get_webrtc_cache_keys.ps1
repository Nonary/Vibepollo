param(
  [string]$RootDir = "",
  [string]$RunnerOs = "",
  [string]$Toolchain = "",
  [string]$Configuration = "",
  [string]$WebrtcRepoUrl = "",
  [string]$WebrtcRef = "",
  [string]$GitExe = "",
  [switch]$AllowUnresolvedRemoteRef
)

$ErrorActionPreference = "Stop"

function Write-Step {
  param([string]$Message)
  Write-Host "[webrtc-cache] $Message"
}

function ConvertTo-CacheToken {
  param([string]$Value)

  if ([string]::IsNullOrWhiteSpace($Value)) {
    return "none"
  }

  $token = $Value.Trim().ToLowerInvariant()
  $token = $token -replace '^https?://', ''
  $token = $token -replace '^git@', ''
  $token = $token -replace ':', '-'
  $token = $token -replace '\.git$', ''
  $token = $token -replace '[^a-z0-9._-]+', '-'
  $token = $token.Trim('-')
  if ([string]::IsNullOrWhiteSpace($token)) {
    return "none"
  }

  if ($token.Length -gt 80) {
    $hash = Get-StringSha256 -Value $token
    return "$($token.Substring(0, 63))-$($hash.Substring(0, 16))"
  }
  return $token
}

function ConvertTo-RepoToken {
  param([string]$RepoUrl)

  $token = $RepoUrl.Trim()
  $token = $token -replace '^https?://', ''
  $token = $token -replace '^git@', ''
  $token = $token -replace '\.git$', ''
  $token = $token -replace '^github\.com[:/]', ''
  $token = $token -replace '^chromium\.googlesource\.com[:/]', 'chromium-'
  return ConvertTo-CacheToken -Value $token
}

function Get-StringSha256 {
  param([string]$Value)

  $sha256 = [System.Security.Cryptography.SHA256]::Create()
  try {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Value)
    return ([System.BitConverter]::ToString($sha256.ComputeHash($bytes))).Replace("-", "").ToLowerInvariant()
  } finally {
    $sha256.Dispose()
  }
}

function Resolve-RemoteGitRef {
  param(
    [string]$RepoUrl,
    [string]$Ref,
    [string]$Git
  )

  if ($Ref -match '^[0-9a-fA-F]{40}$') {
    return $Ref.ToLowerInvariant()
  }

  $queries = @()
  if ($Ref -match '^refs/') {
    $queries += $Ref
  } else {
    $queries += "refs/heads/$Ref"
    $queries += "refs/tags/$Ref^{}"
    $queries += "refs/tags/$Ref"
  }
  $queries += $Ref

  foreach ($query in $queries) {
    Write-Step "Resolving WebRTC ref '$query'"
    $output = & $Git ls-remote $RepoUrl $query 2>$null
    if ($LASTEXITCODE -ne 0) {
      continue
    }

    foreach ($line in @($output)) {
      if ($line -match '^([0-9a-fA-F]{40})\s+') {
        return $matches[1].ToLowerInvariant()
      }
    }
  }

  return ""
}

function Get-GitObject {
  param(
    [string]$Repository,
    [string]$Object,
    [string]$Git
  )

  $value = & $Git -C $Repository rev-parse $Object 2>$null
  if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($value)) {
    throw "Unable to resolve git object '$Object' in $Repository"
  }
  return ($value | Select-Object -First 1).Trim().ToLowerInvariant()
}

function Get-FileSha256 {
  param([string]$Path)

  $sha256 = [System.Security.Cryptography.SHA256]::Create()
  $stream = $null
  try {
    $stream = [System.IO.File]::OpenRead($Path)
    return ([System.BitConverter]::ToString($sha256.ComputeHash($stream))).Replace("-", "").ToLowerInvariant()
  } finally {
    if ($stream) {
      $stream.Dispose()
    }
    $sha256.Dispose()
  }
}

function Get-OutputInputHash {
  param([string]$LibWebrtcDir)

  if (-not (Test-Path -LiteralPath $LibWebrtcDir)) {
    throw "libwebrtc wrapper directory was not found: $LibWebrtcDir"
  }

  $rootPath = (Resolve-Path $LibWebrtcDir).Path
  $inputPaths = @(
    "BUILD.gn",
    "helper.h",
    "include",
    "src"
  )

  $entries = New-Object System.Collections.Generic.List[string]
  foreach ($inputPath in $inputPaths) {
    $absolutePath = Join-Path $rootPath $inputPath
    if (-not (Test-Path -LiteralPath $absolutePath)) {
      continue
    }

    $item = Get-Item -LiteralPath $absolutePath
    $files = if ($item.PSIsContainer) {
      Get-ChildItem -LiteralPath $absolutePath -Recurse -File
    } else {
      @($item)
    }

    foreach ($file in $files) {
      $relativePath = $file.FullName.Substring($rootPath.Length).TrimStart("\", "/").Replace("\", "/")
      $entries.Add("$relativePath`0$(Get-FileSha256 -Path $file.FullName)")
    }
  }

  if ($entries.Count -eq 0) {
    throw "No WebRTC output input files were found under $LibWebrtcDir"
  }

  $sortedEntries = $entries | Sort-Object
  return (Get-StringSha256 -Value ($sortedEntries -join "`n")).Substring(0, 32)
}

function Add-GitHubOutput {
  param(
    [string]$Name,
    [string]$Value
  )

  Write-Host "$Name=$Value"
  if ($env:GITHUB_OUTPUT) {
    "$Name=$Value" >> $env:GITHUB_OUTPUT
  }
}

$scriptRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $RootDir) {
  $RootDir = $scriptRoot
}
$RootDir = (Resolve-Path $RootDir).Path

if (-not $RunnerOs) {
  $RunnerOs = if ($env:RUNNER_OS) { $env:RUNNER_OS } else { [System.Environment]::OSVersion.Platform.ToString() }
}
if (-not $Toolchain) {
  $Toolchain = if ($env:MATRIX_TOOLCHAIN) { $env:MATRIX_TOOLCHAIN } else { "ucrt-x86_64" }
}
if (-not $Configuration) {
  $Configuration = if ($env:WEBRTC_CONFIGURATION) { $env:WEBRTC_CONFIGURATION } else { "Release" }
}
if (-not $WebrtcRepoUrl) {
  $WebrtcRepoUrl = if ($env:WEBRTC_REPO_URL) { $env:WEBRTC_REPO_URL } else { "https://github.com/webrtc-sdk/webrtc.git" }
}
if (-not $WebrtcRef) {
  $WebrtcRef = if ($env:WEBRTC_BRANCH) { $env:WEBRTC_BRANCH } else { "m125_release" }
}

if (-not $GitExe) {
  $gitCommand = Get-Command git -ErrorAction SilentlyContinue
  if ($gitCommand) {
    $GitExe = $gitCommand.Source
  }
}
if (-not $GitExe -or -not (Test-Path $GitExe)) {
  throw "git was not found; set -GitExe or ensure git is in PATH."
}

$cacheSchema = "v2"
$runnerToken = ConvertTo-CacheToken -Value $RunnerOs
$toolchainToken = ConvertTo-CacheToken -Value $Toolchain
$configurationToken = ConvertTo-CacheToken -Value $Configuration
$repoToken = ConvertTo-RepoToken -RepoUrl $WebrtcRepoUrl
$refToken = ConvertTo-CacheToken -Value $WebrtcRef

$sourceRevision = Resolve-RemoteGitRef -RepoUrl $WebrtcRepoUrl -Ref $WebrtcRef -Git $GitExe
if (-not $sourceRevision) {
  if ($AllowUnresolvedRemoteRef) {
    $sourceRevision = "unresolved-$refToken"
  } else {
    throw "Unable to resolve WebRTC ref '$WebrtcRef' from '$WebrtcRepoUrl'. Refusing to create a mutable cache key."
  }
}

$wrapperCommit = Get-GitObject -Repository $RootDir -Object "HEAD:third-party/libwebrtc" -Git $GitExe
$outputInputHash = Get-OutputInputHash -LibWebrtcDir (Join-Path $RootDir "third-party\libwebrtc")

# This is an explicit binary-output recipe, not a hash of the workflow or the
# whole build script. Keep this list aligned with the GN args and copied
# artifacts in build_mingw_webrtc.ps1; bump/edit it only when the produced
# include/ or lib/ WebRTC artifacts should no longer be considered cache
# compatible between Vibeshine and Vibepollo.
$binaryRecipeName = "win-x64-mingw-libwebrtc-dll"
$binaryRecipeInputs = @(
  "schema=$cacheSchema",
  "recipe=$binaryRecipeName",
  "runner=$runnerToken",
  "toolchain=$toolchainToken",
  "configuration=$configurationToken",
  "webrtc_revision=$sourceRevision",
  "libwebrtc_output_inputs=$outputInputHash",
  "target_os=win",
  "target_cpu=x64",
  "is_component_build=false",
  "rtc_use_h264=true",
  "rtc_use_h265=true",
  "ffmpeg_branding=Chrome",
  "rtc_include_tests=false",
  "rtc_build_examples=false",
  "libwebrtc_desktop_capture=true",
  "libwebrtc_intel_media_sdk=true",
  "is_clang=true",
  "clang_use_chrome_plugins=false",
  "use_custom_libcxx=true",
  "artifact=include/libwebrtc.h",
  "artifact=lib/libwebrtc.dll",
  "artifact=lib/libwebrtc.dll.a",
  "artifact=lib/libwebrtc.dll.lib"
)
$binaryRecipeHash = (Get-StringSha256 -Value ($binaryRecipeInputs -join "`n")).Substring(0, 20)

$sourceRestorePrefix = "webrtc-source-$cacheSchema-$runnerToken-$toolchainToken-$repoToken-$refToken-"
$sourceKey = "$sourceRestorePrefix$sourceRevision"
$outputKey = "webrtc-output-$cacheSchema-$runnerToken-$toolchainToken-$configurationToken-$sourceRevision-libwebrtc-inputs-$outputInputHash-$binaryRecipeName-$binaryRecipeHash"

Add-GitHubOutput -Name "source-key" -Value $sourceKey
Add-GitHubOutput -Name "source-restore-prefix" -Value $sourceRestorePrefix
Add-GitHubOutput -Name "output-key" -Value $outputKey
Add-GitHubOutput -Name "webrtc-source-revision" -Value $sourceRevision
Add-GitHubOutput -Name "libwebrtc-wrapper-commit" -Value $wrapperCommit
Add-GitHubOutput -Name "libwebrtc-output-input-hash" -Value $outputInputHash
Add-GitHubOutput -Name "binary-recipe-hash" -Value $binaryRecipeHash
