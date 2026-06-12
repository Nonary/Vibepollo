param(
  [Parameter(Mandatory = $true)]
  [string]$ReleaseTag,
  [string]$Repository = "Nonary/libwebrtc",
  [string]$ManifestPath = ""
)

# Updates the committed pinned-WebRTC manifest from a published Nonary/libwebrtc
# release, analogous to bumping LIBVIRTUALDISPLAY_RELEASE_TAG for the virtual
# display driver. Usage:
#
#   .\scripts\pin_webrtc_release.ps1 -ReleaseTag v1.0.0
#
# The release workflow in Nonary/libwebrtc attaches a windows-x64.json manifest
# (containing the asset name, SHA256, and build provenance) to every release;
# this script downloads it, sanity-checks it, and replaces
# third-party/webrtc-artifacts/windows-x64.json. Commit the result.

$ErrorActionPreference = "Stop"

function Write-Step {
  param([string]$Message)
  Write-Host "[webrtc-pin] $Message"
}

function Require-ManifestValue {
  param(
    [pscustomobject]$Manifest,
    [string]$Name
  )

  $value = $Manifest.$Name
  if ($null -eq $value -or [string]::IsNullOrWhiteSpace([string]$value)) {
    throw "Release manifest is missing required value '$Name'."
  }
  return [string]$value
}

$gh = Get-Command gh -ErrorAction SilentlyContinue
if (-not $gh) {
  throw "GitHub CLI 'gh' was not found."
}

$scriptRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $ManifestPath) {
  $ManifestPath = Join-Path $scriptRoot "third-party\webrtc-artifacts\windows-x64.json"
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) "webrtc-pin-$([System.Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $tempRoot | Out-Null
try {
  Write-Step "Downloading windows-x64.json from $Repository@$ReleaseTag"
  & $gh.Source release download $ReleaseTag --repo $Repository --pattern "windows-x64.json" --dir $tempRoot
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to download the windows-x64.json manifest from release '$Repository@$ReleaseTag'."
  }

  $downloadedManifestPath = Join-Path $tempRoot "windows-x64.json"
  if (-not (Test-Path -LiteralPath $downloadedManifestPath)) {
    throw "Release '$Repository@$ReleaseTag' did not provide a windows-x64.json manifest asset."
  }

  $manifest = Get-Content -LiteralPath $downloadedManifestPath -Raw | ConvertFrom-Json
  $manifestRepo = Require-ManifestValue -Manifest $manifest -Name "repository"
  $manifestTag = Require-ManifestValue -Manifest $manifest -Name "tag"
  $assetName = Require-ManifestValue -Manifest $manifest -Name "asset"
  $sha256 = (Require-ManifestValue -Manifest $manifest -Name "sha256").ToLowerInvariant()
  Require-ManifestValue -Manifest $manifest -Name "output_key" | Out-Null

  if ($manifestRepo -ne $Repository) {
    throw "Manifest repository '$manifestRepo' does not match expected '$Repository'."
  }
  if ($manifestTag -ne $ReleaseTag) {
    throw "Manifest tag '$manifestTag' does not match requested release '$ReleaseTag'."
  }
  if ($sha256 -notmatch '^[0-9a-f]{64}$') {
    throw "Manifest SHA256 '$sha256' is not a valid lowercase hex digest."
  }

  Write-Step "Verifying release asset '$assetName' exists"
  $assetsJson = & $gh.Source release view $ReleaseTag --repo $Repository --json assets
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to list release assets for '$Repository@$ReleaseTag'."
  }
  $assets = @(($assetsJson | ConvertFrom-Json).assets)
  if (-not ($assets | Where-Object { $_.name -eq $assetName })) {
    throw "Release '$Repository@$ReleaseTag' does not contain asset '$assetName'."
  }

  Copy-Item -LiteralPath $downloadedManifestPath -Destination $ManifestPath -Force
  Write-Step "Pinned $Repository@$ReleaseTag ($assetName, sha256=$sha256)"
  Write-Step "Updated $ManifestPath - review and commit the change."
} finally {
  if (Test-Path -LiteralPath $tempRoot) {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
  }
}
