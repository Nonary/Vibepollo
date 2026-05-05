param(
  [string]$ManifestPath,
  [string]$AssetPath,
  [string]$NotesPath = "",
  [switch]$AllowReplace
)

$ErrorActionPreference = "Stop"

function Write-Step {
  param([string]$Message)
  Write-Host "[webrtc-publish] $Message"
}

function Require-Value {
  param(
    [pscustomobject]$Object,
    [string]$Name
  )

  $value = $Object.$Name
  if ($null -eq $value -or [string]::IsNullOrWhiteSpace([string]$value)) {
    throw "WebRTC release manifest is missing required value '$Name'."
  }
  return [string]$value
}

if (-not $ManifestPath -or -not (Test-Path -LiteralPath $ManifestPath)) {
  throw "ManifestPath was not found: $ManifestPath"
}
if (-not $AssetPath -or -not (Test-Path -LiteralPath $AssetPath)) {
  throw "AssetPath was not found: $AssetPath"
}

$gh = Get-Command gh -ErrorAction SilentlyContinue
if (-not $gh) {
  throw "GitHub CLI 'gh' was not found."
}

$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$repo = Require-Value -Object $manifest -Name "repository"
$tag = Require-Value -Object $manifest -Name "tag"
$assetName = Require-Value -Object $manifest -Name "asset"
$expectedSha256 = (Require-Value -Object $manifest -Name "sha256").ToLowerInvariant()

if ((Split-Path -Leaf $AssetPath) -ne $assetName) {
  throw "AssetPath leaf name does not match manifest asset '$assetName'."
}

$actualSha256 = (Get-FileHash -LiteralPath $AssetPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualSha256 -ne $expectedSha256) {
  throw "Local WebRTC asset SHA256 mismatch. Manifest expects $expectedSha256, file is $actualSha256."
}

$releaseTitle = "WebRTC Windows x64 $tag"
$releaseExists = $false
$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
  & $gh.Source release view $tag --repo $repo *> $null
  if ($LASTEXITCODE -eq 0) {
    $releaseExists = $true
  }
} finally {
  $ErrorActionPreference = $previousErrorActionPreference
}

if (-not $releaseExists) {
  Write-Step "Creating release $repo@$tag"
  $createArgs = @("release", "create", $tag, "--repo", $repo, "--title", $releaseTitle)
  if ($NotesPath -and (Test-Path -LiteralPath $NotesPath)) {
    $createArgs += @("--notes-file", $NotesPath)
  } else {
    $createArgs += @("--notes", "Pinned Windows x64 WebRTC artifacts for Vibepollo.")
  }
  if ($env:GITHUB_SHA) {
    $createArgs += @("--target", $env:GITHUB_SHA)
  }
  & $gh.Source @createArgs
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to create release '$tag' in '$repo'."
  }
} else {
  Write-Step "Release $repo@$tag already exists"
}

$existingAssetsJson = & $gh.Source release view $tag --repo $repo --json assets
if ($LASTEXITCODE -ne 0) {
  throw "Failed to list release assets for '$repo@$tag'."
}
$existingAssets = @($existingAssetsJson | ConvertFrom-Json | Select-Object -ExpandProperty assets)
$existingAsset = @($existingAssets | Where-Object { $_.name -eq $assetName } | Select-Object -First 1)

if ($existingAsset.Count -gt 0) {
  $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) "vibepollo-webrtc-publish-$([System.Guid]::NewGuid().ToString('N'))"
  New-Item -ItemType Directory -Path $tempRoot | Out-Null
  try {
    & $gh.Source release download $tag --repo $repo --pattern $assetName --dir $tempRoot
    if ($LASTEXITCODE -ne 0) {
      throw "Failed to download existing release asset '$assetName'."
    }

    $existingPath = Join-Path $tempRoot $assetName
    $existingSha256 = (Get-FileHash -LiteralPath $existingPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($existingSha256 -eq $expectedSha256) {
      Write-Step "Release asset '$assetName' already exists with the expected SHA256; nothing to upload."
      return
    }

    if (-not $AllowReplace) {
      throw "Existing release asset '$assetName' has SHA256 $existingSha256, expected $expectedSha256. Refusing to replace without -AllowReplace."
    }
  } finally {
    if (Test-Path -LiteralPath $tempRoot) {
      Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
  }

  Write-Step "Replacing existing release asset $assetName"
  & $gh.Source release delete-asset $tag $assetName --repo $repo --yes
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to delete existing release asset '$assetName'."
  }
}

Write-Step "Uploading $assetName"
& $gh.Source release upload $tag $AssetPath --repo $repo
if ($LASTEXITCODE -ne 0) {
  throw "Failed to upload WebRTC release asset '$assetName'."
}
