[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Repository,
    [Parameter(Mandatory = $true)][string] $Tag,
    [Parameter(Mandatory = $true)][string] $OutDir,
    [Parameter(Mandatory = $true)][string] $ExpectedArchiveSha256,
    [Parameter(Mandatory = $true)][string] $ExpectedSourceRevision,
    [Parameter(Mandatory = $true)][string] $ExpectedDriverVer,
    [Parameter(Mandatory = $true)][uint16] $ExpectedProtocolVersion,
    [switch] $ValidateOnly,
    [string] $GitHubToken
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$consumerLockName = 'consumer-release-lock.json'
$expectedLayout = @(
    'driver/VibeshineVhfGamepad.cat',
    'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.inf',
    'manifest.json',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)
$expectedManifestFiles = @(
    'driver/VibeshineVhfGamepad.cat',
    'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.inf',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)
$expectedSignedDownstream = @(
    'driver/VibeshineVhfGamepad.cat',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)
$expectedUnsignedPayloads = @(
    'driver/VibeshineVhfGamepad.cat',
    'driver/VibeshineVhfGamepad.dll',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)
$expectedMembershipFiles = @(
    'driver/VibeshineVhfGamepad.cat',
    'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.inf'
)

function Write-Step {
    param([Parameter(Mandatory = $true)][string] $Message)
    Write-Host "[libvirtualgamepad] $Message"
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string] $Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-ExactList {
    param(
        [Parameter(Mandatory = $true)][object[]] $Actual,
        [Parameter(Mandatory = $true)][object[]] $Expected,
        [Parameter(Mandatory = $true)][string] $Name
    )
    $actualText = @($Actual | ForEach-Object { [string] $_ }) -join "`n"
    $expectedText = @($Expected | ForEach-Object { [string] $_ }) -join "`n"
    if ($actualText -cne $expectedText) {
        throw "$Name does not match the pinned producer contract.`nExpected:`n$expectedText`nActual:`n$actualText"
    }
}

function Get-RelativeFiles {
    param([Parameter(Mandatory = $true)][string] $Root)
    $rootPath = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    return @(
        Get-ChildItem -LiteralPath $rootPath -Recurse -File | ForEach-Object {
            $_.FullName.Substring($rootPath.Length).TrimStart('\', '/').Replace('\', '/')
        } | Sort-Object
    )
}

function Assert-UnsignedAuthenticode {
    param([Parameter(Mandatory = $true)][string] $Path)
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::NotSigned -or
        $null -ne $signature.SignerCertificate) {
        $signer = if ($null -eq $signature.SignerCertificate) { '<none>' } else { $signature.SignerCertificate.Subject }
        throw "Producer payload '$Path' is not unsigned (status=$($signature.Status), signer=$signer)."
    }
}

function New-GitHubHeaders {
    param([string] $Token, [string] $Accept = 'application/vnd.github+json')
    $headers = @{
        Accept = $Accept
        'User-Agent' = 'libvirtualgamepad-consumer-release-fetcher'
        'X-GitHub-Api-Version' = '2022-11-28'
    }
    if (-not [string]::IsNullOrWhiteSpace($Token)) {
        $headers.Authorization = "Bearer $Token"
    }
    return $headers
}

function Assert-JsonReleaseContract {
    param(
        [Parameter(Mandatory = $true)] $Metadata,
        [Parameter(Mandatory = $true)][string] $Name
    )
    if ($null -eq $Metadata -or $Metadata.schema_version -ne 1 -or
        [string] $Metadata.tag -cne $Tag -or
        [string] $Metadata.tag_target -cne $ExpectedSourceRevision -or
        [string] $Metadata.source_revision -cne $ExpectedSourceRevision -or
        [string] $Metadata.driver_ver -cne $ExpectedDriverVer -or
        [uint16] $Metadata.protocol_version -ne $ExpectedProtocolVersion -or
        [string] $Metadata.platform -cne 'x64' -or
        [string] $Metadata.archive.name -cne $script:archiveName -or
        [string] $Metadata.archive.sha256 -cne $ExpectedArchiveSha256 -or
        [string] $Metadata.manifest.path -cne 'manifest.json' -or
        [string]::IsNullOrWhiteSpace([string] $Metadata.manifest.sha256) -or
        [string] $Metadata.signing.channel -cne 'msi-request-signing') {
        throw "$Name metadata does not match the pinned producer release."
    }
    Assert-ExactList -Actual @($Metadata.layout) -Expected $expectedLayout -Name "$Name layout"
    Assert-ExactList -Actual @($Metadata.signing.signed_downstream) -Expected $expectedSignedDownstream -Name "$Name signed_downstream"
    Assert-ExactList -Actual @($Metadata.signing.unsigned_payloads) -Expected $expectedUnsignedPayloads -Name "$Name unsigned_payloads"
    if ([string] $Metadata.signing.catalog_membership.basis -cne 'fresh-inf2cat' -or
        [string] $Metadata.signing.catalog_membership.generator -cne 'Inf2Cat') {
        throw "$Name does not prove fresh Inf2Cat catalog construction."
    }
    Assert-ExactList -Actual @($Metadata.signing.catalog_membership.files.PSObject.Properties.Name | Sort-Object) -Expected $expectedMembershipFiles -Name "$Name catalog membership files"
}

function Assert-ProducerReleaseDirectory {
    param([Parameter(Mandatory = $true)][string] $Root)
    $producerDir = Join-Path $Root 'producer-release'
    $archivePath = Join-Path $producerDir $script:archiveName
    $checksumPath = Join-Path $producerDir $script:checksumName
    $lockPath = Join-Path $producerDir $script:producerLockName
    $evidencePath = Join-Path $producerDir $script:evidenceName
    foreach ($path in @($archivePath, $checksumPath, $lockPath, $evidencePath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -le 0) {
            throw "Pinned producer release file is missing or empty: $path"
        }
    }
    if ((Get-Sha256 -Path $archivePath) -cne $ExpectedArchiveSha256) {
        throw 'Producer archive hash does not match the consumer pin.'
    }
    $checksumText = (Get-Content -LiteralPath $checksumPath -Raw).TrimEnd("`r", "`n")
    if ($checksumText -cne "$ExpectedArchiveSha256  $script:archiveName") {
        throw 'Producer SHA-256 sidecar does not exactly identify the pinned archive.'
    }

    $producerLock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
    $evidence = Get-Content -LiteralPath $evidencePath -Raw | ConvertFrom-Json
    Assert-JsonReleaseContract -Metadata $producerLock -Name 'Producer release lock'
    Assert-JsonReleaseContract -Metadata $evidence -Name 'Producer evidence'
    if ([string] $producerLock.manifest.sha256 -cne [string] $evidence.manifest.sha256) {
        throw 'Producer lock and evidence disagree about the manifest hash.'
    }
    foreach ($property in @(
        'clean_tagged_head', 'infverif_required', 'inf2cat_required',
        'fresh_inf2cat_evidence', 'package_verified_unsigned_for_msi_signing',
        'unsigned_authenticode_checked', 'certificate_absent', 'archive_created_once'
    )) {
        $evidenceProperty = $evidence.verification.PSObject.Properties[$property]
        if ($null -eq $evidenceProperty -or $evidenceProperty.Value -ne $true) {
            throw "Producer evidence does not affirm '$property'."
        }
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        Assert-ExactList -Actual @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') } | Sort-Object) -Expected $expectedLayout -Name 'Case-sensitive ZIP layout'
        if (@($zip.Entries | Group-Object FullName | Where-Object Count -ne 1).Count -ne 0) {
            throw 'Producer ZIP contains duplicate entry names.'
        }
        foreach ($entry in $zip.Entries) {
            if ($entry.FullName -match '(^|/|\\)\.\.($|/|\\)|^[A-Za-z]:|^[/\\]') {
                throw "Producer ZIP contains an unsafe path: $($entry.FullName)"
            }
        }
        $manifestEntry = @($zip.Entries | Where-Object { $_.FullName -ceq 'manifest.json' })
        if ($manifestEntry.Count -ne 1) {
            throw 'Producer ZIP does not contain exactly one case-correct manifest.json.'
        }
        $stream = $manifestEntry[0].Open()
        try {
            $hasher = [System.Security.Cryptography.SHA256]::Create()
            try {
                $archivedManifestHash = ([System.BitConverter]::ToString($hasher.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
            } finally {
                $hasher.Dispose()
            }
        } finally {
            $stream.Dispose()
        }
        if ($archivedManifestHash -cne [string] $producerLock.manifest.sha256) {
            throw 'Archived manifest hash does not match producer sidecars.'
        }
    } finally {
        $zip.Dispose()
    }

    $manifestPath = Join-Path $Root 'manifest.json'
    $manifestHash = Get-Sha256 -Path $manifestPath
    if ($manifestHash -cne [string] $producerLock.manifest.sha256) {
        throw 'Staged manifest hash does not match producer sidecars.'
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema_version -ne 1 -or
        [string] $manifest.source_revision -cne $ExpectedSourceRevision -or
        [string] $manifest.driver_ver -cne $ExpectedDriverVer -or
        [uint16] $manifest.protocol_version -ne $ExpectedProtocolVersion -or
        [string] $manifest.platform -cne 'x64' -or
        [string] $manifest.signing.channel -cne 'msi-request-signing') {
        throw 'Producer manifest does not match the pinned source, version, platform, and signing channel.'
    }
    Assert-ExactList -Actual @($manifest.files | ForEach-Object { [string] $_.path } | Sort-Object) -Expected $expectedManifestFiles -Name 'Manifest file list'
    Assert-ExactList -Actual @($manifest.signing.signed_downstream) -Expected $expectedSignedDownstream -Name 'Manifest signed_downstream'
    Assert-ExactList -Actual @($manifest.signing.unsigned_payloads) -Expected $expectedUnsignedPayloads -Name 'Manifest unsigned_payloads'
    if ([string] $manifest.signing.catalog_membership.basis -cne 'fresh-inf2cat' -or
        [string] $manifest.signing.catalog_membership.generator -cne 'Inf2Cat') {
        throw 'Producer manifest does not bind the payload to fresh Inf2Cat construction.'
    }
    Assert-ExactList -Actual @($manifest.signing.catalog_membership.files.PSObject.Properties.Name | Sort-Object) -Expected $expectedMembershipFiles -Name 'Manifest catalog membership files'

    foreach ($entry in @($manifest.files)) {
        $relative = [string] $entry.path
        $path = Join-Path $Root ($relative -replace '/', '\')
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -le 0) {
            throw "Producer payload is missing or empty: $relative"
        }
        $actualHash = Get-Sha256 -Path $path
        if ($actualHash -cne [string] $entry.sha256) {
            throw "Producer manifest hash mismatch: $relative"
        }
        if ($relative -cin $expectedMembershipFiles) {
            $membershipHash = [string] $manifest.signing.catalog_membership.files.PSObject.Properties[$relative].Value
            if ($membershipHash -cne $actualHash) {
                throw "Fresh Inf2Cat evidence hash mismatch: $relative"
            }
            $lockMembershipHash = [string] $producerLock.signing.catalog_membership.files.PSObject.Properties[$relative].Value
            $evidenceMembershipHash = [string] $evidence.signing.catalog_membership.files.PSObject.Properties[$relative].Value
            if ($lockMembershipHash -cne $actualHash -or $evidenceMembershipHash -cne $actualHash) {
                throw "Producer sidecars disagree with the manifest membership hash: $relative"
            }
        }
    }
    foreach ($relative in $expectedUnsignedPayloads) {
        Assert-UnsignedAuthenticode -Path (Join-Path $Root ($relative -replace '/', '\'))
    }
    if (@(Get-ChildItem -LiteralPath $Root -Recurse -File -Filter '*.cer').Count -ne 0) {
        throw 'Producer package must not contain a certificate.'
    }

    $consumerLockPath = Join-Path $Root $consumerLockName
    if (-not (Test-Path -LiteralPath $consumerLockPath -PathType Leaf)) {
        throw "Consumer release lock is missing: $consumerLockPath"
    }
    $consumerLock = Get-Content -LiteralPath $consumerLockPath -Raw | ConvertFrom-Json
    if ($consumerLock.schema_version -ne 1 -or
        [string] $consumerLock.repository -cne $Repository -or
        [string] $consumerLock.tag -cne $Tag -or
        [string] $consumerLock.tag_target -cne $ExpectedSourceRevision -or
        [string] $consumerLock.asset_name -cne $script:archiveName -or
        [string] $consumerLock.archive_sha256 -cne $ExpectedArchiveSha256 -or
        [string] $consumerLock.manifest_sha256 -cne $manifestHash -or
        [string] $consumerLock.driver_ver -cne $ExpectedDriverVer -or
        [uint16] $consumerLock.protocol_version -ne $ExpectedProtocolVersion -or
        [string] $consumerLock.signing_channel -cne 'msi-request-signing') {
        throw 'Consumer release lock does not match the pinned producer release.'
    }
    Assert-ExactList -Actual @($consumerLock.layout) -Expected $expectedLayout -Name 'Consumer lock layout'
    Assert-ExactList -Actual @($consumerLock.signed_downstream) -Expected $expectedSignedDownstream -Name 'Consumer lock signed_downstream'
    foreach ($name in $script:expectedAssetNames) {
        $property = $consumerLock.producer_asset_sha256.PSObject.Properties[$name]
        $actualHash = Get-Sha256 -Path (Join-Path $producerDir $name)
        if ($null -eq $property -or [string] $property.Value -cne $actualHash) {
            throw "Consumer lock producer-asset hash mismatch: $name"
        }
    }

    $expectedStagedFiles = @(
        $consumerLockName
        $expectedLayout
        "producer-release/$script:archiveName"
        "producer-release/$script:checksumName"
        "producer-release/$script:evidenceName"
        "producer-release/$script:producerLockName"
    ) | ForEach-Object { $_ } | Sort-Object
    Assert-ExactList -Actual (Get-RelativeFiles -Root $Root) -Expected $expectedStagedFiles -Name 'Verified consumer artifact layout'
}

if ($Repository -cne 'Nonary/libvirtualgamepad') {
    throw 'Repository must be the public Nonary/libvirtualgamepad producer.'
}
if ($Tag -notmatch '^v0\.1\.0-beta\.[1-9][0-9]*$') {
    throw 'Tag does not match the supported libvirtualgamepad release line.'
}
if ($ExpectedArchiveSha256 -notmatch '^[0-9a-fA-F]{64}$') {
    throw 'ExpectedArchiveSha256 must be a 64-character SHA-256 value.'
}
if ($ExpectedSourceRevision -notmatch '^[0-9a-fA-F]{40}$') {
    throw 'ExpectedSourceRevision must be a 40-character commit SHA.'
}
$ExpectedArchiveSha256 = $ExpectedArchiveSha256.ToLowerInvariant()
$ExpectedSourceRevision = $ExpectedSourceRevision.ToLowerInvariant()
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
$version = $Tag.Substring(1)
$assetBase = "libvirtualgamepad-$version-windows-x64"
$archiveName = "$assetBase.zip"
$checksumName = "$archiveName.sha256"
$producerLockName = "$assetBase.release-lock.json"
$evidenceName = "$assetBase-evidence.json"
$expectedAssetNames = @($archiveName, $checksumName, $evidenceName, $producerLockName) | Sort-Object

if ($ValidateOnly) {
    Assert-ProducerReleaseDirectory -Root $OutDir
    Write-Step "Validated pinned producer release artifact at $OutDir"
    exit 0
}
if (Test-Path -LiteralPath $OutDir) {
    Assert-ProducerReleaseDirectory -Root $OutDir
    Write-Step "Using verified pinned producer release at $OutDir"
    exit 0
}
if (-not $GitHubToken) {
    $GitHubToken = if ($env:GH_TOKEN) { $env:GH_TOKEN } elseif ($env:GITHUB_TOKEN) { $env:GITHUB_TOKEN } else { '' }
}

$escapedTag = [System.Uri]::EscapeDataString($Tag)
$release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repository/releases/tags/$escapedTag" -Headers (New-GitHubHeaders -Token $GitHubToken)
if ($release.tag_name -cne $Tag -or $release.target_commitish -cne $ExpectedSourceRevision -or
    $release.draft -ne $false -or $release.prerelease -ne $true -or $release.immutable -ne $true) {
    throw 'GitHub release is not the exact immutable public prerelease pinned by the consumer.'
}
$tagRef = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repository/git/ref/tags/$escapedTag" -Headers (New-GitHubHeaders -Token $GitHubToken)
if ($tagRef.ref -cne "refs/tags/$Tag" -or $tagRef.object.type -cne 'commit' -or $tagRef.object.sha -cne $ExpectedSourceRevision) {
    throw 'Producer tag is not a lightweight commit ref at the pinned source revision.'
}
$assets = @($release.assets)
Assert-ExactList -Actual @($assets | ForEach-Object { [string] $_.name } | Sort-Object) -Expected $expectedAssetNames -Name 'GitHub release asset set'
foreach ($asset in $assets) {
    if ([int64] $asset.size -le 0 -or [string]::IsNullOrWhiteSpace([string] $asset.url) -or
        [string] $asset.digest -notmatch '^sha256:[0-9a-f]{64}$') {
        throw "GitHub asset '$($asset.name)' lacks a usable immutable size, URL, or digest."
    }
}

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) "libvirtualgamepad-consumer-$([System.Guid]::NewGuid().ToString('N'))"
$downloadDir = Join-Path $temporaryRoot 'download'
$extractDir = Join-Path $temporaryRoot 'extract'
$stageDir = "$OutDir.partial-$([System.Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $downloadDir, $extractDir, $stageDir -Force | Out-Null
try {
    foreach ($asset in $assets) {
        $destination = Join-Path $downloadDir ([string] $asset.name)
        Write-Step "Downloading $($asset.name)"
        Invoke-WebRequest -Uri ([string] $asset.url) -Headers (New-GitHubHeaders -Token $GitHubToken -Accept 'application/octet-stream') -OutFile $destination
        if ((Get-Item -LiteralPath $destination).Length -ne [int64] $asset.size) {
            throw "Downloaded asset size mismatch: $($asset.name)"
        }
        if ("sha256:$(Get-Sha256 -Path $destination)" -cne [string] $asset.digest) {
            throw "Downloaded asset digest mismatch: $($asset.name)"
        }
    }

    $producerStageDir = Join-Path $stageDir 'producer-release'
    New-Item -ItemType Directory -Path $producerStageDir -Force | Out-Null
    foreach ($name in $expectedAssetNames) {
        Copy-Item -LiteralPath (Join-Path $downloadDir $name) -Destination (Join-Path $producerStageDir $name) -Force
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $downloadedArchive = Join-Path $downloadDir $archiveName
    $zip = [System.IO.Compression.ZipFile]::OpenRead($downloadedArchive)
    try {
        Assert-ExactList -Actual @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') } | Sort-Object) -Expected $expectedLayout -Name 'Downloaded case-sensitive ZIP layout'
        if (@($zip.Entries | Group-Object FullName | Where-Object Count -ne 1).Count -ne 0) {
            throw 'Downloaded producer ZIP contains duplicate entry names.'
        }
        foreach ($entry in $zip.Entries) {
            if ($entry.FullName -match '(^|/|\\)\.\.($|/|\\)|^[A-Za-z]:|^[/\\]') {
                throw "Downloaded producer ZIP contains an unsafe path: $($entry.FullName)"
            }
        }
    } finally {
        $zip.Dispose()
    }
    [System.IO.Compression.ZipFile]::ExtractToDirectory($downloadedArchive, $extractDir)
    foreach ($relative in $expectedLayout) {
        $source = Join-Path $extractDir ($relative -replace '/', '\')
        $destination = Join-Path $stageDir ($relative -replace '/', '\')
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }

    $producerLock = Get-Content -LiteralPath (Join-Path $downloadDir $producerLockName) -Raw | ConvertFrom-Json
    $producerAssetHashes = [ordered]@{}
    foreach ($name in $expectedAssetNames) {
        $producerAssetHashes[$name] = Get-Sha256 -Path (Join-Path $downloadDir $name)
    }
    $consumerLock = [ordered]@{
        schema_version = 1
        repository = $Repository
        tag = $Tag
        tag_target = $ExpectedSourceRevision
        asset_name = $archiveName
        archive_sha256 = $ExpectedArchiveSha256
        manifest_sha256 = [string] $producerLock.manifest.sha256
        driver_ver = $ExpectedDriverVer
        protocol_version = $ExpectedProtocolVersion
        platform = 'x64'
        layout = $expectedLayout
        signing_channel = 'msi-request-signing'
        signed_downstream = $expectedSignedDownstream
        producer_asset_sha256 = $producerAssetHashes
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $stageDir $consumerLockName),
        ($consumerLock | ConvertTo-Json -Depth 5),
        [System.Text.UTF8Encoding]::new($false))
    Assert-ProducerReleaseDirectory -Root $stageDir
    Move-Item -LiteralPath $stageDir -Destination $OutDir
    Write-Step "Staged immutable $Repository release $Tag at $OutDir"
} finally {
    if (Test-Path -LiteralPath $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
