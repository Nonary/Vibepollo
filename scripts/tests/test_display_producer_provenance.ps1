param([string] $RepositoryRoot = (Join-Path $PSScriptRoot '../..'))
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$downloader = Join-Path $RepositoryRoot 'scripts/download_libvirtualdisplay_release.ps1'
$realLockPath = Join-Path $RepositoryRoot 'third-party/libvirtualdisplay-release/windows-x64.json'
$realLock = Get-Content $realLockPath -Raw | ConvertFrom-Json
function Assert-That($Condition,[string] $Message) { if (-not $Condition) { throw $Message } }
function Assert-Fails([scriptblock] $Action,[string] $Pattern) {
    $message = ''
    try { & $Action } catch { $message = $_.Exception.Message }
    Assert-That ($message -match $Pattern) "Expected failure '$Pattern', got '$message'"
}
function Hash([string] $Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('display-provenance-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $fixture = Join-Path $testRoot $realLock.package_root
    $names = @('driver/SunshineVirtualDisplayDriver.inf','driver/SunshineVirtualDisplayDriver.dll','driver/SunshineVirtualDisplayDriver.cat',
        'driver/SunshineVirtualDisplayDriver.cer','tools/virtualdisplay_probe.exe','vulkan-layer/VkLayer_sunshine_hdr.dll','vulkan-layer/VkLayer_sunshine_hdr.json')
    $files = [ordered]@{}
    foreach ($name in $names) {
        $path = Join-Path $fixture $name
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($path)) -Force | Out-Null
        if ($name.EndsWith('.inf')) { 'DriverVer=10/04/2026,1.6.4.0' | Set-Content $path } else { "pure provenance fixture: $name" | Set-Content $path }
        $files[$name] = Hash $path
    }
    $fixtureArchivePath = Join-Path $testRoot $realLock.archive_name
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($fixtureArchivePath,[IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $names) {
            $entry = $zip.CreateEntry("$($realLock.package_root)/$name")
            $inputStream = [IO.File]::OpenRead((Join-Path $fixture $name)); $outputStream = $entry.Open()
            try { $inputStream.CopyTo($outputStream) } finally { $inputStream.Dispose(); $outputStream.Dispose() }
        }
    } finally { $zip.Dispose() }
    $lock = $realLock | ConvertTo-Json -Depth 6 | ConvertFrom-Json
    $lock.archive_sha256 = Hash $fixtureArchivePath
    $lock.files = [pscustomobject]$files
    $fixtureEvidencePath = Join-Path $testRoot $lock.evidence_name
    [ordered]@{tag=$lock.tag;commit_sha=$lock.source_revision;package=$lock.archive_name;package_sha256=$lock.archive_sha256;
        signing=@{channel='self-signed';certificate_thumbprint=$lock.certificate_thumbprint};driver_catalog='self-signed'} |
        ConvertTo-Json | Set-Content $fixtureEvidencePath
    $lock.evidence_sha256 = Hash $fixtureEvidencePath
    $fixtureLock = Join-Path $testRoot 'fixture-lock.json'
    $lock | ConvertTo-Json -Depth 6 | Set-Content $fixtureLock
    $fixtureRelease = [pscustomobject]@{tag_name=$lock.tag;draft=$false;assets=@(
        [pscustomobject]@{name=$lock.archive_name;digest="sha256:$($lock.archive_sha256)";url='https://api.github.com/repos/Nonary/libvirtualdisplay/releases/assets/1'},
        [pscustomobject]@{name=$lock.evidence_name;digest="sha256:$($lock.evidence_sha256)";url='https://api.github.com/repos/Nonary/libvirtualdisplay/releases/assets/2'})}
    $fixtureTagRef = [pscustomobject]@{ref="refs/tags/$($lock.tag)";object=[pscustomobject]@{type='tag';sha=('a'*40)}}
    $fixtureTagObject = [pscustomobject]@{sha=('a'*40);tag=$lock.tag;object=[pscustomobject]@{type='commit';sha=$lock.source_revision}}
    $mockState = @{ reads = 0; badArchive = $false }
    function Invoke-RestMethod($Uri,$Headers) {
        $mockState.reads++
        if ($Uri -match '/releases/tags/') { return $fixtureRelease }
        if ($Uri -match '/git/ref/tags/') { return $fixtureTagRef }
        if ($Uri -match '/git/tags/') { return $fixtureTagObject }
        throw "Unexpected network fixture read $Uri"
    }
    function Invoke-WebRequest($Uri,$Headers,$OutFile) {
        $mockState.reads++
        if ($Uri -eq 'https://api.github.com/repos/Nonary/libvirtualdisplay/releases/assets/1') {
            if ($mockState.badArchive) { 'invalid archive bytes' | Set-Content $OutFile } else { Copy-Item $fixtureArchivePath $OutFile }
        } elseif ($Uri -eq 'https://api.github.com/repos/Nonary/libvirtualdisplay/releases/assets/2') { Copy-Item $fixtureEvidencePath $OutFile }
        else { throw "Unexpected network fixture download $Uri" }
    }
    $cache = Join-Path $testRoot 'cache'
    $downloadArguments = @{Repository=$lock.repository;Tag=$lock.tag;OutDir=$cache;LockPath=$fixtureLock;GitHubToken=''}
    & $downloader @downloadArguments
    Assert-That ($mockState.reads -eq 5) 'Fresh producer must validate release/tag and both downloads'
    $mockState.reads = 0
    & $downloader @downloadArguments -ValidateOnly
    Assert-That ($mockState.reads -eq 0) 'Cached exact payload validation must be offline'
    foreach ($name in $names) {
        $path = Join-Path $cache $name
        $original = [IO.File]::ReadAllBytes($path)
        try { 'tampered component' | Set-Content $path; Assert-Fails { & $downloader @downloadArguments -ValidateOnly } 'hash mismatch' }
        finally { [IO.File]::WriteAllBytes($path,$original) }
    }
    $extra = Join-Path $cache 'unexpected.exe'
    'extra' | Set-Content $extra
    Assert-Fails { & $downloader @downloadArguments -ValidateOnly } 'inventory'
    Remove-Item -LiteralPath $extra
    foreach ($name in @('consumer-release-lock.json','producer-evidence.json')) {
        $path = Join-Path $cache $name
        $original = [IO.File]::ReadAllBytes($path)
        try {
            $data = Get-Content $path -Raw | ConvertFrom-Json
            if ($name -eq 'consumer-release-lock.json') { $data.source_revision = 'b'*40 } else { $data.commit_sha = 'b'*40 }
            $data | ConvertTo-Json -Depth 6 | Set-Content $path
            Assert-Fails { & $downloader @downloadArguments -ValidateOnly } 'metadata|evidence hash'
        } finally { [IO.File]::WriteAllBytes($path,$original) }
    }
    $freshArguments = $downloadArguments.Clone(); $freshArguments.OutDir = Join-Path $testRoot 'absent'
    Assert-Fails { & $downloader @freshArguments -ValidateOnly } 'absent'
    $fixtureTagObject.object.sha = 'b'*40
    Assert-Fails { & $downloader @freshArguments } 'tag moved'
    $fixtureTagObject.object.sha = $lock.source_revision
    $fixtureRelease.assets[0].digest = 'sha256:'+('b'*64)
    Assert-Fails { & $downloader @freshArguments } 'asset digest'
    $fixtureRelease.assets[0].digest = 'sha256:'+$lock.archive_sha256
    $originalUrl = $fixtureRelease.assets[0].url
    $fixtureRelease.assets[0].url = 'https://unrelated.invalid/asset'
    Assert-Fails { & $downloader @freshArguments } 'asset API URL'
    $fixtureRelease.assets[0].url = $originalUrl
    $mockState.badArchive = $true
    Assert-Fails { & $downloader @freshArguments } 'archive SHA-256'
    $mockState.badArchive = $false
    # Detect a source/version mismatch even when a fixture lock's file hash is made consistent.
    $inf = Join-Path $cache 'driver/SunshineVirtualDisplayDriver.inf'
    $originalInf = [IO.File]::ReadAllBytes($inf)
    try {
        'DriverVer=10/04/2026,1.6.3.0' | Set-Content $inf
        $lock.files.'driver/SunshineVirtualDisplayDriver.inf' = Hash $inf
        $lock | ConvertTo-Json -Depth 6 | Set-Content $fixtureLock
        Assert-Fails { & $downloader @downloadArguments -ValidateOnly } 'INF DriverVer'
    } finally { [IO.File]::WriteAllBytes($inf,$originalInf) }
    $refreshPath = Join-Path $RepositoryRoot 'packaging/windows/virtual_display_driver/refresh_driver_package.ps1'
    $refreshAst = [Management.Automation.Language.Parser]::ParseFile($refreshPath,[ref]$null,[ref]$null)
    $versionFunction = $refreshAst.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Resolve-PackageVersionFromPrebuiltRoot'},$true)
    . ([scriptblock]::Create($versionFunction.Extent.Text))
    Assert-That ((Resolve-PackageVersionFromPrebuiltRoot -Path $cache) -ceq '1.6.4.0') 'Reviewed metadata must supply real component version regardless of cache directory name'
    $refresh = Get-Content $refreshPath -Raw
    $gate = $refresh.IndexOf('& $producerVerifier -Repository')
    Assert-That ($gate -gt 0 -and $gate -lt $refresh.IndexOf('$resolvedPackageVersion = Resolve-PackageVersion')) 'Refresh must verify producer before version resolution/copy/build'
    Assert-That ($realLock.tag -ceq 'v1.6.4' -and $realLock.source_revision -ceq '2a29ea71a52d15522c95b3de532b6612c54d75cc' -and
        $realLock.archive_sha256 -ceq '34c6cddb507187b9a0971765644b38637004d7ebed104e830363f36b661ed075' -and
        $realLock.driver_ver -ceq '10/04/2026,1.6.4.0') 'Reviewed production display identity must remain exact'
    $cmake = Get-Content (Join-Path $RepositoryRoot 'cmake/packaging/windows.cmake') -Raw
    Assert-That ($cmake.Contains('-LockPath "${SUNSHINE_VDD_LIBVIRTUALDISPLAY_RELEASE_LOCK}"') -and
        $cmake.Contains('set(_sunshine_vdd_provenance_args -ValidateOnly)')) 'CMake must verify explicit prebuilt roots too'
    $contract = Get-Content (Join-Path $RepositoryRoot 'cmake/packaging/windows_virtual_display_contract.cmake') -Raw
    Assert-That ($contract.Contains('string(JSON SUNSHINE_VDD_LIBVIRTUALDISPLAY_RELEASE_TAG GET')) 'Display declaration must read release tag from same lock'
    Write-Host 'Display producer provenance tests passed: fresh/cache, every payload tamper, metadata, tag, digest, archive, inventory and real INF version. Network mocked; no keys/trust/installers/services.'
} finally {
    $resolved = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $testRoot).Path)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    if (-not $resolved.Equals([IO.Path]::GetFullPath($testRoot),[StringComparison]::OrdinalIgnoreCase) -or
        -not $resolved.StartsWith($temp,[StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notmatch '^display-provenance-test-[0-9a-f]{32}$') { throw 'Refusing unsafe provenance test cleanup.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
