[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Repository,
    [Parameter(Mandatory = $true)][string] $Tag,
    [Parameter(Mandatory = $true)][string] $OutDir,
    [string] $LockPath = '',
    [switch] $ValidateOnly,
    [string] $GitHubToken = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Get-Sha256([string] $Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function New-GitHubHeaders([string] $Token = '') {
    $headers = @{ Accept = 'application/vnd.github+json'; 'User-Agent' = 'vibepollo-libvirtualdisplay-provenance'; 'X-GitHub-Api-Version' = '2022-11-28' }
    if ($Token) { $headers.Authorization = "Bearer $Token" }
    return $headers
}
function Assert-DisplayLock($Lock) {
    if ($Lock.schema_version -ne 1 -or $Lock.repository -cne $Repository -or $Lock.tag -cne $Tag -or
        $Lock.source_revision -notmatch '^[0-9a-f]{40}$' -or $Lock.archive_sha256 -notmatch '^[0-9a-f]{64}$' -or
        $Lock.evidence_sha256 -notmatch '^[0-9a-f]{64}$' -or $Lock.driver_ver -notmatch '^\d{2}/\d{2}/\d{4},\d+\.\d+\.\d+\.\d+$' -or
        $Lock.signing_channel -cne 'self-signed' -or $Lock.certificate_thumbprint -notmatch '^[0-9A-F]{40}$') {
        throw 'Display producer lock does not match requested repository, release or component identity.'
    }
    $expectedRoot = 'libvirtualdisplay-' + $Tag.TrimStart('v') + '-windows-x64'
    if ($Lock.package_root -cne $expectedRoot -or $Lock.archive_name -cne "$expectedRoot.zip" -or
        $Lock.evidence_name -cne ('libvirtualdisplay-' + $Tag.TrimStart('v') + '-evidence.json')) { throw 'Unexpected display asset names.' }
    $names = @($Lock.files.PSObject.Properties.Name)
    if ($names.Count -lt 7 -or @($names | Select-Object -Unique).Count -ne $names.Count) { throw 'Display file lock is incomplete.' }
    foreach ($name in $names) {
        if ($name -notmatch '^[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*$' -or $name -match '(^|/)\.\.?(/|$)' -or
            $Lock.files.$name -notmatch '^[0-9a-f]{64}$') { throw 'Unsafe display lock path or invalid digest.' }
    }
    foreach ($required in @('driver/SunshineVirtualDisplayDriver.inf','driver/SunshineVirtualDisplayDriver.dll',
        'driver/SunshineVirtualDisplayDriver.cat','driver/SunshineVirtualDisplayDriver.cer',
        'tools/virtualdisplay_probe.exe','vulkan-layer/VkLayer_sunshine_hdr.dll','vulkan-layer/VkLayer_sunshine_hdr.json')) {
        if ($required -cnotin $names) { throw "Display lock omits $required." }
    }
}
function Assert-DisplayEvidence([string] $Path, $Lock) {
    if ((Get-Sha256 $Path) -cne $Lock.evidence_sha256) { throw 'Display evidence hash differs from reviewed lock.' }
    $e = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($e.tag -cne $Lock.tag -or $e.commit_sha -cne $Lock.source_revision -or $e.package -cne $Lock.archive_name -or
        $e.package_sha256 -cne $Lock.archive_sha256 -or $e.signing.channel -cne $Lock.signing_channel -or
        $e.signing.certificate_thumbprint -cne $Lock.certificate_thumbprint -or $e.driver_catalog -cne 'self-signed') {
        throw 'Display evidence does not identify pinned source, archive and signing channel.'
    }
}
function Assert-DisplayPayload([string] $Path, $Lock, [switch] $Cached) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { throw 'Display producer directory is missing.' }
    if ((Get-Item -LiteralPath $Path).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Display root is a reparse point.' }
    $root = [IO.Path]::GetFullPath($Path).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    $items = @(Get-ChildItem -LiteralPath $Path -Recurse -Force)
    if (@($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Display payload contains a reparse point.' }
    $actual = @($items | Where-Object { -not $_.PSIsContainer } | ForEach-Object { $_.FullName.Substring($root.Length).Replace('\','/') } | Sort-Object)
    $expected = @($Lock.files.PSObject.Properties.Name)
    if ($Cached) { $expected += @('producer-evidence.json','consumer-release-lock.json') }
    $expected = @($expected | Sort-Object)
    if (($actual -join "`n") -cne ($expected -join "`n")) { throw 'Display file inventory differs from reviewed lock.' }
    foreach ($p in $Lock.files.PSObject.Properties) {
        if ((Get-Sha256 (Join-Path $Path $p.Name)) -cne $p.Value) { throw "Display payload hash mismatch: $($p.Name)" }
    }
    $inf = Get-Content -LiteralPath (Join-Path $Path 'driver/SunshineVirtualDisplayDriver.inf') -Raw
    $versions = [regex]::Matches($inf,'(?m)^\s*DriverVer\s*=\s*([^\r\n]+)')
    if ($versions.Count -ne 1 -or $versions[0].Groups[1].Value.Trim() -cne $Lock.driver_ver) { throw 'Display INF DriverVer differs from actual locked component.' }
    if ($Cached) {
        Assert-DisplayEvidence (Join-Path $Path 'producer-evidence.json') $Lock
        $c = Get-Content -LiteralPath (Join-Path $Path 'consumer-release-lock.json') -Raw | ConvertFrom-Json
        if ($c.schema_version -ne 1 -or $c.repository -cne $Lock.repository -or $c.tag -cne $Lock.tag -or
            $c.source_revision -cne $Lock.source_revision -or $c.archive_sha256 -cne $Lock.archive_sha256 -or
            $c.evidence_sha256 -cne $Lock.evidence_sha256 -or $c.driver_ver -cne $Lock.driver_ver -or
            $c.reviewed_lock_sha256 -cne (Get-Sha256 $LockPath)) { throw 'Cached display metadata differs from reviewed lock.' }
    }
}
function Assert-DisplayRelease($Release,$TagRef,$TagObject,$Lock) {
    if ($Release.tag_name -cne $Lock.tag -or $Release.draft -ne $false) { throw 'Display release metadata mismatch.' }
    # This release is mutable and annotated; target_commitish="master" is not source evidence.
    if ($TagRef.ref -cne "refs/tags/$($Lock.tag)") { throw 'Unexpected display tag ref.' }
    $target = $TagRef.object
    if ($target.type -ceq 'tag') {
        if ($null -eq $TagObject -or $TagObject.sha -cne $target.sha -or $TagObject.tag -cne $Lock.tag) { throw 'Display annotated tag mismatch.' }
        $target = $TagObject.object
    }
    if ($target.type -cne 'commit' -or $target.sha -cne $Lock.source_revision) { throw 'Display tag moved away from reviewed source.' }
    foreach ($spec in @(@{name=$Lock.archive_name;hash=$Lock.archive_sha256},@{name=$Lock.evidence_name;hash=$Lock.evidence_sha256})) {
        $assets = @($Release.assets | Where-Object { $_.name -ceq $spec.name })
        if ($assets.Count -ne 1) { throw 'Display release must have exactly one pinned archive and evidence asset.' }
        if ($assets[0].digest -cne "sha256:$($spec.hash)") { throw 'Display release asset digest differs from reviewed lock.' }
        if ($assets[0].url -notmatch ('^https://api\.github\.com/repos/'+[regex]::Escape($Repository)+'/releases/assets/[0-9]+$')) { throw 'Unexpected display asset API URL.' }
    }
}
function Remove-DisplayScratch([string] $Path,[string] $Parent,[string] $Prefix) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $resolved = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Path).Path)
    $parentPath = [IO.Path]::GetFullPath($Parent).TrimEnd('\','/')
    if (-not $resolved.Equals([IO.Path]::GetFullPath($Path),[StringComparison]::OrdinalIgnoreCase) -or
        -not [IO.Path]::GetDirectoryName($resolved).Equals($parentPath,[StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notmatch ('^'+[regex]::Escape($Prefix)+'[0-9a-f]{32}$') -or
        ((Get-Item -LiteralPath $resolved).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Refusing unsafe display scratch cleanup.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
if (-not $LockPath) { $LockPath = Join-Path $PSScriptRoot '../third-party/libvirtualdisplay-release/windows-x64.json' }
$lock = Get-Content -LiteralPath $LockPath -Raw | ConvertFrom-Json
Assert-DisplayLock $lock
$OutDir = [IO.Path]::GetFullPath($OutDir)
if (Test-Path -LiteralPath $OutDir) {
    Assert-DisplayPayload $OutDir $lock -Cached
    Write-Host "[libvirtualdisplay] Verified reviewed producer bytes at $OutDir (self-signed; OS trust is separate)."
    exit 0
}
if ($ValidateOnly) { throw 'Display cache is absent; validation never downloads or repairs it.' }
if (-not $GitHubToken) { $GitHubToken = if ($env:GH_TOKEN) { $env:GH_TOKEN } elseif ($env:GITHUB_TOKEN) { $env:GITHUB_TOKEN } else { '' } }
$headers = New-GitHubHeaders $GitHubToken
$release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repository/releases/tags/$Tag" -Headers $headers
$tagRef = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repository/git/ref/tags/$Tag" -Headers $headers
$tagObject = $null
if ($tagRef.object.type -ceq 'tag') { $tagObject = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repository/git/tags/$($tagRef.object.sha)" -Headers $headers }
Assert-DisplayRelease $release $tagRef $tagObject $lock
$tempParent = [IO.Path]::GetTempPath()
$tempRoot = Join-Path $tempParent ('display-producer-'+[guid]::NewGuid().ToString('N'))
$stageParent = [IO.Path]::GetDirectoryName($OutDir)
$stagePrefix = [IO.Path]::GetFileName($OutDir)+'.partial-'
$stageDir = Join-Path $stageParent ($stagePrefix+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
try {
    foreach ($name in @($lock.archive_name,$lock.evidence_name)) {
        $asset = @($release.assets | Where-Object { $_.name -ceq $name })[0]
        $downloadHeaders = New-GitHubHeaders $GitHubToken
        $downloadHeaders.Accept = 'application/octet-stream'
        Invoke-WebRequest -Uri $asset.url -Headers $downloadHeaders -OutFile (Join-Path $tempRoot $name)
    }
    $archivePath = Join-Path $tempRoot $lock.archive_name
    if ((Get-Sha256 $archivePath) -cne $lock.archive_sha256) { throw 'Display archive SHA-256 mismatch.' }
    $evidencePath = Join-Path $tempRoot $lock.evidence_name
    Assert-DisplayEvidence $evidencePath $lock
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $entries = @($archive.Entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName } | Sort-Object)
        $expected = @($lock.files.PSObject.Properties.Name | ForEach-Object { "$($lock.package_root)/$_" } | Sort-Object)
        if (($entries -join "`n") -cne ($expected -join "`n")) { throw 'Display archive inventory differs from reviewed lock.' }
        foreach ($entry in $archive.Entries) {
            if ($entry.FullName.Contains('\') -or $entry.FullName -match '(^|/)\.\.?(/|$)' -or
                -not $entry.FullName.StartsWith("$($lock.package_root)/",[StringComparison]::Ordinal)) { throw 'Unsafe display archive entry.' }
        }
    } finally { $archive.Dispose() }
    $extractDir = Join-Path $tempRoot 'extract'
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractDir
    $payload = Join-Path $extractDir $lock.package_root
    Assert-DisplayPayload $payload $lock
    New-Item -ItemType Directory -Path $stageDir | Out-Null
    Get-ChildItem -LiteralPath $payload -Force | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $stageDir -Recurse }
    Copy-Item -LiteralPath $evidencePath -Destination (Join-Path $stageDir 'producer-evidence.json')
    [ordered]@{schema_version=1;repository=$lock.repository;tag=$lock.tag;source_revision=$lock.source_revision;
        archive_sha256=$lock.archive_sha256;evidence_sha256=$lock.evidence_sha256;driver_ver=$lock.driver_ver;
        reviewed_lock_sha256=(Get-Sha256 $LockPath)} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stageDir 'consumer-release-lock.json') -Encoding UTF8
    Assert-DisplayPayload $stageDir $lock -Cached
    if (Test-Path -LiteralPath $OutDir) { throw 'Display cache appeared during staging; preserving it.' }
    Move-Item -LiteralPath $stageDir -Destination $OutDir
    Write-Host "[libvirtualdisplay] Staged reviewed $Repository $Tag bytes (self-signed; not an OS trust approval)."
} finally {
    Remove-DisplayScratch $stageDir $stageParent $stagePrefix
    Remove-DisplayScratch $tempRoot $tempParent 'display-producer-'
}
