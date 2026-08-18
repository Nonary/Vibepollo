[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Repository,

    [Parameter(Mandatory = $true)]
    [string] $Tag,

    [Parameter(Mandatory = $true)]
    [string] $OutDir,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedArchiveSha256,

    [string] $GitHubToken
)

$ErrorActionPreference = 'Stop'
$releaseCacheLockFile = '.sunshine-release-asset-lock.json'

function Write-Step {
    param([Parameter(Mandatory = $true)][string] $Message)
    Write-Host "[libvirtualgamepad] $Message"
}

function New-GitHubHeaders {
    param([string] $Token)

    $headers = @{ 'User-Agent' = 'Vibeshine-libvirtualgamepad-release-fetcher' }
    if (-not [string]::IsNullOrWhiteSpace($Token)) {
        $headers['Authorization'] = "Bearer $Token"
    }
    return $headers
}

function Test-PackageRoot {
    param([Parameter(Mandatory = $true)][string] $Path)

    foreach ($relativePath in @(
        'driver\VibeshineVhfGamepad.inf',
        'driver\VibeshineVhfGamepad.dll',
        'driver\VibeshineVhfGamepad.cat',
        'tools\VibeshineVhfGamepadDeviceSetup.exe',
        'manifest.json'
    )) {
        $item = Get-Item -LiteralPath (Join-Path $Path $relativePath) -ErrorAction SilentlyContinue
        if ($null -eq $item -or $item.Length -le 0) {
            return $false
        }
    }
    return $true
}

function Test-VerifiedReleaseCache {
    param(
        [Parameter(Mandatory = $true)][string] $Path,
        [Parameter(Mandatory = $true)][string] $Repository,
        [Parameter(Mandatory = $true)][string] $Tag,
        [Parameter(Mandatory = $true)][string] $AssetName,
        [Parameter(Mandatory = $true)][string] $ArchiveSha256
    )

    if (-not (Test-PackageRoot -Path $Path)) {
        return $false
    }
    $lockPath = Join-Path $Path $releaseCacheLockFile
    if (-not (Test-Path -LiteralPath $lockPath -PathType Leaf)) {
        return $false
    }
    try {
        $lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
        return $null -ne $lock -and $lock.schema_version -eq 1 -and
            $lock.repository -eq $Repository -and $lock.tag -eq $Tag -and
            $lock.asset_name -eq $AssetName -and
            $lock.archive_sha256 -eq $ArchiveSha256
    } catch {
        return $false
    }
}

function Write-VerifiedReleaseCacheLock {
    param(
        [Parameter(Mandatory = $true)][string] $Path,
        [Parameter(Mandatory = $true)][string] $Repository,
        [Parameter(Mandatory = $true)][string] $Tag,
        [Parameter(Mandatory = $true)][string] $AssetName,
        [Parameter(Mandatory = $true)][string] $ArchiveSha256
    )

    $lock = [ordered]@{
        schema_version = 1
        repository = $Repository
        tag = $Tag
        asset_name = $AssetName
        archive_sha256 = $ArchiveSha256
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $Path $releaseCacheLockFile),
        ($lock | ConvertTo-Json -Depth 3),
        [System.Text.UTF8Encoding]::new($false))
}

if ([string]::IsNullOrWhiteSpace($Repository)) {
    throw 'libvirtualgamepad repository is required.'
}
if ([string]::IsNullOrWhiteSpace($Tag)) {
    throw 'libvirtualgamepad release tag is required.'
}
if ($ExpectedArchiveSha256 -notmatch '^[0-9a-fA-F]{64}$') {
    throw 'ExpectedArchiveSha256 must be a 64-character SHA-256 value.'
}
$ExpectedArchiveSha256 = $ExpectedArchiveSha256.ToLowerInvariant()

$OutDir = [System.IO.Path]::GetFullPath($OutDir)
$version = $Tag -replace '^v', ''
$assetName = "libvirtualgamepad-$version-windows-x64.zip"
if (-not $GitHubToken) {
    $GitHubToken = if ($env:GH_TOKEN) {
        $env:GH_TOKEN
    } elseif ($env:GITHUB_TOKEN) {
        $env:GITHUB_TOKEN
    } else {
        ''
    }
}

if (Test-VerifiedReleaseCache -Path $OutDir -Repository $Repository -Tag $Tag -AssetName $assetName -ArchiveSha256 $ExpectedArchiveSha256) {
    Write-Step "Using staged $Repository release $Tag from $OutDir"
    exit 0
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
    Write-Step 'Authenticated release lookup failed; retrying public lookup without a token'
    $GitHubToken = ''
    $headers = New-GitHubHeaders
    $release = Invoke-RestMethod -Uri $releaseUri -Headers $headers
}

$asset = @($release.assets | Where-Object { $_.name -eq $assetName } | Select-Object -First 1)
if ($asset.Count -ne 1) {
    throw "Release '$Repository@$Tag' does not contain asset '$assetName'."
}

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) "libvirtualgamepad-$([System.Guid]::NewGuid().ToString('N'))"
$downloadDir = Join-Path $temporaryRoot 'download'
$extractDir = Join-Path $temporaryRoot 'extract'
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

    $actualArchiveSha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualArchiveSha256 -ne $ExpectedArchiveSha256) {
        throw "Downloaded $assetName SHA-256 does not match the pinned release asset. Expected $ExpectedArchiveSha256, got $actualArchiveSha256."
    }

    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractDir -Force
    $packageRoot = @(
        Get-Item -LiteralPath $extractDir
        Get-ChildItem -LiteralPath $extractDir -Recurse -Directory
    ) | Where-Object { Test-PackageRoot -Path $_.FullName } | Select-Object -First 1
    if ($null -eq $packageRoot) {
        throw "Release archive '$assetName' does not contain the required libvirtualgamepad package layout."
    }

    Get-ChildItem -LiteralPath $packageRoot.FullName -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $stageDir -Recurse -Force
    }
    if (-not (Test-PackageRoot -Path $stageDir)) {
        throw "Staged libvirtualgamepad payload is incomplete: $stageDir"
    }
    Write-VerifiedReleaseCacheLock -Path $stageDir -Repository $Repository -Tag $Tag -AssetName $assetName -ArchiveSha256 $ExpectedArchiveSha256

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
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
