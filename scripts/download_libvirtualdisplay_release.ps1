param(
    [Parameter(Mandatory = $true)]
    [string]$Repository,

    [Parameter(Mandatory = $true)]
    [string]$Tag,

    [Parameter(Mandatory = $true)]
    [string]$OutDir,

    [string]$GitHubToken = ''
)

$ErrorActionPreference = 'Stop'

function Write-Step {
    param([Parameter(Mandatory = $true)][string]$Message)
    Write-Host "[libvirtualdisplay] $Message"
}

function New-GitHubHeaders {
    param([string]$Token = '')

    $headers = @{
        'Accept' = 'application/vnd.github+json'
        'User-Agent' = 'vibepollo-libvirtualdisplay-downloader'
        'X-GitHub-Api-Version' = '2022-11-28'
    }
    if ($Token) {
        $headers['Authorization'] = "Bearer $Token"
    }
    return $headers
}

function Test-PackageRoot {
    param([Parameter(Mandatory = $true)][string]$Path)

    foreach ($relativePath in @(
        'driver\SunshineVirtualDisplayDriver.dll',
        'driver\SunshineVirtualDisplayDriver.inf',
        'driver\SunshineVirtualDisplayDriver.cat',
        'tools\virtualdisplay_probe.exe',
        'vulkan-layer\VkLayer_sunshine_hdr.dll',
        'vulkan-layer\VkLayer_sunshine_hdr.json'
    )) {
        $item = Get-Item -LiteralPath (Join-Path $Path $relativePath) -ErrorAction SilentlyContinue
        if (-not $item -or $item.Length -le 0) {
            return $false
        }
    }
    return $true
}

if ([string]::IsNullOrWhiteSpace($Repository)) {
    throw 'libvirtualdisplay repository is required.'
}
if ([string]::IsNullOrWhiteSpace($Tag)) {
    throw 'libvirtualdisplay release tag is required.'
}

$OutDir = [System.IO.Path]::GetFullPath($OutDir)
if (-not $GitHubToken) {
    $GitHubToken = if ($env:GH_TOKEN) {
        $env:GH_TOKEN
    } elseif ($env:GITHUB_TOKEN) {
        $env:GITHUB_TOKEN
    } else {
        ''
    }
}

if (Test-PackageRoot -Path $OutDir) {
    Write-Step "Using staged $Repository release $Tag from $OutDir"
    exit 0
}

$version = $Tag -replace '^v', ''
$assetName = "libvirtualdisplay-$version-windows-x64.zip"
$headers = New-GitHubHeaders -Token $GitHubToken
$releaseUri = "https://api.github.com/repos/$Repository/releases/tags/$([System.Uri]::EscapeDataString($Tag))"
Write-Step "Resolving $Repository release $Tag"
try {
    $release = Invoke-RestMethod -Uri $releaseUri -Headers $headers
} catch {
    if (-not $GitHubToken) {
        throw
    }
    Write-Step 'Authenticated release lookup failed; retrying public lookup without a token'
    $GitHubToken = ''
    $headers = New-GitHubHeaders
    $release = Invoke-RestMethod -Uri $releaseUri -Headers $headers
}

$asset = @($release.assets | Where-Object { $_.name -eq $assetName } | Select-Object -First 1)
if ($asset.Count -ne 1) {
    throw "Release '$Repository@$Tag' does not contain asset '$assetName'."
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) "libvirtualdisplay-$([System.Guid]::NewGuid().ToString('N'))"
$downloadDir = Join-Path $tempRoot 'download'
$extractDir = Join-Path $tempRoot 'extract'
$stageDir = "$OutDir.partial-$([System.Guid]::NewGuid().ToString('N'))"
$backupDir = "$OutDir.previous-$([System.Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $downloadDir, $extractDir, $stageDir -Force | Out-Null

try {
    $archivePath = Join-Path $downloadDir $assetName
    $downloadHeaders = New-GitHubHeaders -Token $GitHubToken
    $downloadHeaders['Accept'] = 'application/octet-stream'
    Write-Step "Downloading $assetName"
    try {
        Invoke-WebRequest -Uri $asset.url -Headers $downloadHeaders -OutFile $archivePath
    } catch {
        if (-not $GitHubToken) {
            throw
        }
        Write-Step 'Authenticated asset download failed; retrying public download without a token'
        $downloadHeaders = New-GitHubHeaders
        $downloadHeaders['Accept'] = 'application/octet-stream'
        Invoke-WebRequest -Uri $asset.url -Headers $downloadHeaders -OutFile $archivePath
    }

    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractDir -Force
    $packageRoot = @(
        Get-Item -LiteralPath $extractDir
        Get-ChildItem -LiteralPath $extractDir -Recurse -Directory
    ) | Where-Object { Test-PackageRoot -Path $_.FullName } | Select-Object -First 1
    if (-not $packageRoot) {
        throw "Release archive '$assetName' does not contain the required libvirtualdisplay driver payload."
    }

    Get-ChildItem -LiteralPath $packageRoot.FullName -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $stageDir -Recurse -Force
    }
    if (-not (Test-PackageRoot -Path $stageDir)) {
        throw "Staged libvirtualdisplay payload is incomplete: $stageDir"
    }

    if (Test-Path -LiteralPath $OutDir) {
        Move-Item -LiteralPath $OutDir -Destination $backupDir
    }
    try {
        Move-Item -LiteralPath $stageDir -Destination $OutDir
    } catch {
        if ((Test-Path -LiteralPath $backupDir) -and -not (Test-Path -LiteralPath $OutDir)) {
            Move-Item -LiteralPath $backupDir -Destination $OutDir
        }
        throw
    }
    if (Test-Path -LiteralPath $backupDir) {
        Remove-Item -LiteralPath $backupDir -Recurse -Force
    }

    Write-Step "Staged $Repository release $Tag at $OutDir"
} finally {
    if (Test-Path -LiteralPath $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
