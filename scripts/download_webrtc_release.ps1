param(
  [string]$ManifestPath = "",
  [string]$OutDir = "",
  [string]$GitHubToken = ""
)

$ErrorActionPreference = "Stop"

function Write-Step {
  param([string]$Message)
  Write-Host "[webrtc-download] $Message"
}

function Require-ManifestValue {
  param(
    [pscustomobject]$Manifest,
    [string]$Name
  )

  $value = $Manifest.$Name
  if ($null -eq $value -or [string]::IsNullOrWhiteSpace([string]$value)) {
    throw "Pinned WebRTC manifest is missing required value '$Name'."
  }
  return [string]$value
}

function New-GitHubHeaders {
  param([string]$Token = "")

  $headers = @{
    "Accept" = "application/vnd.github+json"
    "User-Agent" = "vibepollo-webrtc-downloader"
    "X-GitHub-Api-Version" = "2022-11-28"
  }

  if ($Token) {
    $headers["Authorization"] = "Bearer $Token"
  }

  return $headers
}

$scriptRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $ManifestPath) {
  $ManifestPath = Join-Path $scriptRoot "third-party\webrtc-artifacts\windows-x64.json"
}
if (-not (Test-Path -LiteralPath $ManifestPath)) {
  throw "Pinned WebRTC manifest was not found: $ManifestPath"
}
$ManifestPath = (Resolve-Path $ManifestPath).Path

if (-not $OutDir) {
  $OutDir = if ($env:WEBRTC_ROOT) { $env:WEBRTC_ROOT } else { Join-Path $scriptRoot ".vibepollo-deps\libwebrtc\out" }
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

if (-not $GitHubToken) {
  $GitHubToken = if ($env:GH_TOKEN) { $env:GH_TOKEN } elseif ($env:GITHUB_TOKEN) { $env:GITHUB_TOKEN } else { "" }
}

$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$repo = Require-ManifestValue -Manifest $manifest -Name "repository"
$tag = Require-ManifestValue -Manifest $manifest -Name "tag"
$assetName = Require-ManifestValue -Manifest $manifest -Name "asset"
$expectedSha256 = (Require-ManifestValue -Manifest $manifest -Name "sha256").ToLowerInvariant()
$expectedOutputKey = Require-ManifestValue -Manifest $manifest -Name "output_key"

if ($expectedSha256 -notmatch '^[0-9a-f]{64}$') {
  throw "Pinned WebRTC manifest has invalid SHA256 '$expectedSha256'. Refusing an unverified download."
}

$headers = New-GitHubHeaders -Token $GitHubToken
$releaseUri = "https://api.github.com/repos/$repo/releases/tags/$([System.Uri]::EscapeDataString($tag))"
Write-Step "Resolving $repo release $tag"
try {
  $release = Invoke-RestMethod -Uri $releaseUri -Headers $headers
} catch {
  if (-not $GitHubToken) {
    throw
  }

  Write-Step "Authenticated release lookup failed; retrying public lookup without a token"
  $headers = New-GitHubHeaders
  $release = Invoke-RestMethod -Uri $releaseUri -Headers $headers
}
$asset = @($release.assets | Where-Object { $_.name -eq $assetName } | Select-Object -First 1)
if ($asset.Count -eq 0) {
  throw "Release '$repo@$tag' does not contain pinned asset '$assetName'."
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) "vibepollo-webrtc-$([System.Guid]::NewGuid().ToString('N'))"
$downloadDir = Join-Path $tempRoot "download"
$extractDir = Join-Path $tempRoot "extract"
New-Item -ItemType Directory -Path $downloadDir, $extractDir | Out-Null

try {
  $archivePath = Join-Path $downloadDir $assetName
  $downloadHeaders = New-GitHubHeaders -Token $GitHubToken
  $downloadHeaders["Accept"] = "application/octet-stream"

  Write-Step "Downloading $assetName"
  try {
    Invoke-WebRequest -Uri $asset.url -Headers $downloadHeaders -OutFile $archivePath
  } catch {
    if (-not $GitHubToken) {
      throw
    }

    Write-Step "Authenticated asset download failed; retrying public download without a token"
    $downloadHeaders = New-GitHubHeaders
    $downloadHeaders["Accept"] = "application/octet-stream"
    Invoke-WebRequest -Uri $asset.url -Headers $downloadHeaders -OutFile $archivePath
  }

  $actualSha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actualSha256 -ne $expectedSha256) {
    throw "Pinned WebRTC SHA256 mismatch for '$assetName'. Expected $expectedSha256, got $actualSha256."
  }

  Write-Step "Verified $assetName ($actualSha256)"
  Expand-Archive -LiteralPath $archivePath -DestinationPath $extractDir -Force

  $artifactManifestPath = Join-Path $extractDir "webrtc-artifact.json"
  if (-not (Test-Path -LiteralPath $artifactManifestPath)) {
    throw "Downloaded WebRTC archive is missing webrtc-artifact.json."
  }

  $artifactManifest = Get-Content -LiteralPath $artifactManifestPath -Raw | ConvertFrom-Json
  $actualOutputKey = Require-ManifestValue -Manifest $artifactManifest -Name "output_key"
  if ($actualOutputKey -ne $expectedOutputKey) {
    throw "Downloaded WebRTC output key mismatch. Expected '$expectedOutputKey', got '$actualOutputKey'."
  }

  foreach ($requiredPath in @(
      "include\libwebrtc.h",
      "lib\libwebrtc.dll",
      "lib\libwebrtc.dll.a"
    )) {
    $path = Join-Path $extractDir $requiredPath
    if (-not (Test-Path -LiteralPath $path)) {
      throw "Downloaded WebRTC archive is missing required path '$requiredPath'."
    }
  }

  if (Test-Path -LiteralPath $OutDir) {
    Remove-Item -LiteralPath $OutDir -Recurse -Force
  }
  New-Item -ItemType Directory -Path $OutDir | Out-Null
  Copy-Item -Path (Join-Path $extractDir "*") -Destination $OutDir -Recurse -Force

  Write-Step "Installed pinned WebRTC artifacts to $OutDir"
} finally {
  if (Test-Path -LiteralPath $tempRoot) {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
  }
}
