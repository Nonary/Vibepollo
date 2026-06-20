param(
  [string]$Repository = "Nonary/vibeshine_truehdr_runtime",
  [string]$Tag = "v1.0.0",
  [string]$AssetName = "",
  [string]$OutDir = "",
  [string]$GitHubToken = ""
)

$ErrorActionPreference = "Stop"

function Write-Step {
  param([string]$Message)
  Write-Host "[truehdr-runtime] $Message"
}

function New-GitHubHeaders {
  param([string]$Token = "")

  $headers = @{
    "Accept" = "application/vnd.github+json"
    "User-Agent" = "vibeshine-truehdr-runtime-downloader"
    "X-GitHub-Api-Version" = "2022-11-28"
  }

  if ($Token) {
    $headers["Authorization"] = "Bearer $Token"
  }

  return $headers
}

if ([string]::IsNullOrWhiteSpace($Repository)) {
  throw "TrueHDR runtime repository is required."
}
if ([string]::IsNullOrWhiteSpace($Tag)) {
  throw "TrueHDR runtime release tag is required."
}

$scriptRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $OutDir) {
  $OutDir = if ($env:SUNSHINE_TRUEHDR_RUNTIME_DIR) {
    $env:SUNSHINE_TRUEHDR_RUNTIME_DIR
  } else {
    Join-Path $scriptRoot ".vibeshine-deps\truehdr-runtime"
  }
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

if (-not $GitHubToken) {
  $GitHubToken = if ($env:TRUEHDR_RUNTIME_TOKEN) {
    $env:TRUEHDR_RUNTIME_TOKEN
  } elseif ($env:GH_TOKEN) {
    $env:GH_TOKEN
  } elseif ($env:GITHUB_TOKEN) {
    $env:GITHUB_TOKEN
  } else {
    ""
  }
}

$version = $Tag -replace '^v', ''
if ([string]::IsNullOrWhiteSpace($AssetName)) {
  $AssetName = "vibeshine-truehdr-runtime-$version-windows-x64.zip"
}

$headers = New-GitHubHeaders -Token $GitHubToken
$releaseUri = "https://api.github.com/repos/$Repository/releases/tags/$([System.Uri]::EscapeDataString($Tag))"
Write-Step "Resolving $Repository release $Tag"
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

$asset = @($release.assets | Where-Object { $_.name -eq $AssetName } | Select-Object -First 1)
if ($asset.Count -eq 0) {
  throw "Release '$Repository@$Tag' does not contain asset '$AssetName'."
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) "vibeshine-truehdr-$([System.Guid]::NewGuid().ToString('N'))"
$downloadDir = Join-Path $tempRoot "download"
$extractDir = Join-Path $tempRoot "extract"
New-Item -ItemType Directory -Path $downloadDir, $extractDir | Out-Null

try {
  $archivePath = Join-Path $downloadDir $AssetName
  $downloadHeaders = New-GitHubHeaders -Token $GitHubToken
  $downloadHeaders["Accept"] = "application/octet-stream"

  Write-Step "Downloading $AssetName"
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

  Expand-Archive -LiteralPath $archivePath -DestinationPath $extractDir -Force

  $requiredDlls = @("vibeshine_truehdr.dll", "nvngx_truehdr.dll")
  $resolved = @{}
  foreach ($dll in $requiredDlls) {
    $matches = @(Get-ChildItem -LiteralPath $extractDir -Recurse -File -Filter $dll)
    if ($matches.Count -ne 1) {
      Get-ChildItem -LiteralPath $extractDir -Recurse -File | ForEach-Object { Write-Host $_.FullName }
      throw "Expected exactly one '$dll' in TrueHDR runtime archive, found $($matches.Count)."
    }
    if ($matches[0].Length -le 0) {
      throw "TrueHDR runtime file is empty: $($matches[0].FullName)"
    }
    $resolved[$dll] = $matches[0].FullName
  }

  if (Test-Path -LiteralPath $OutDir) {
    Remove-Item -LiteralPath $OutDir -Recurse -Force
  }
  New-Item -ItemType Directory -Path $OutDir | Out-Null

  foreach ($dll in $requiredDlls) {
    Copy-Item -LiteralPath $resolved[$dll] -Destination (Join-Path $OutDir $dll) -Force
  }

  Write-Step "Installed pinned TrueHDR runtime to $OutDir"
  Get-ChildItem -LiteralPath $OutDir -File | ForEach-Object {
    Write-Step ("{0}  {1} bytes  sha256={2}" -f $_.Name, $_.Length, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())
  }
} finally {
  if (Test-Path -LiteralPath $tempRoot) {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
  }
}
