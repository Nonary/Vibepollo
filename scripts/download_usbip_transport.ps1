# Extract original signed driver files; do not execute the vendor installer.
# Its upgrade path first uninstalls the existing shared transport and reconnects
# USB root hubs. Vibepollo deliberately never invokes that path.
param(
    [Parameter(Mandatory=$true)][string]$OutDir,
    [string]$CacheDir = ''
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
. (Join-Path $PSScriptRoot '..\packaging\windows\usbip_transport\package.ps1')
if (!$CacheDir) { $CacheDir = Join-Path (Split-Path -Parent $OutDir) 'usbip-downloads' }

function Get-VerifiedFile([string]$Url, [string]$Path, [string]$Hash) {
    if (Test-Path -LiteralPath $Path) {
        if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Hash) {
            throw "Cached download has an unexpected SHA-256: $Path"
        }
        return
    }
    $partial = "$Path.partial"
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $partial
        if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $Hash) {
            throw "Downloaded file failed SHA-256 verification: $Url"
        }
        Move-Item -LiteralPath $partial -Destination $Path
    } finally {
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
    }
}

function Resolve-SignTool {
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $candidate = Get-ChildItem -LiteralPath $kits -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (!$candidate) { throw 'Windows SDK signtool.exe is required to verify the signed USB/IP catalogs and their INF/SYS members.' }
    return $candidate
}

function Assert-CatalogMembers([string]$Directory, [string]$SignTool) {
    foreach ($driver in @('usbip2_ude', 'usbip2_filter')) {
        foreach ($extension in @('inf', 'sys')) {
            & $SignTool verify /kp /c (Join-Path $Directory "$driver.cat") (Join-Path $Directory "$driver.$extension")
            if ($LASTEXITCODE -ne 0) { throw "USB/IP catalog membership or kernel signing verification failed: $driver.$extension" }
        }
    }
}

$signtool = Resolve-SignTool
# Re-extract even when output exists: a cached output manifest is not proof
# that its filter files originated in the pinned installer.
New-Item -ItemType Directory -Path $CacheDir -Force | Out-Null
$installer = Join-Path $CacheDir $UsbIpInstallerName
Get-VerifiedFile $UsbIpInstallerUrl $installer $UsbIpInstallerHash
$extractorZip = Join-Path $CacheDir 'innounp-2.71.1.zip'
Get-VerifiedFile 'https://raw.githubusercontent.com/jrathlev/InnoUnpacker-Windows-GUI/6fb49264aacf512a093e7b4fc6fb3dd266dad31a/innounp-2/bin/innounp-2.zip' $extractorZip 'f2f037fdbc63de31248efae9ccb294398d160dc0cbad5f36d14c2f159e17bbf5'
$work = Join-Path $CacheDir ('extract-' + [Guid]::NewGuid().ToString('N'))
try {
    $extractorDir = Join-Path $work 'unpacker'
    Expand-Archive -LiteralPath $extractorZip -DestinationPath $extractorDir
    $extractor = Join-Path $extractorDir 'innounp.exe'
    $extracted = Join-Path $work 'drivers'
    $patterns = @($UsbIpFiles | ForEach-Object { '{tmp}\' + $_ })
    & $extractor -x -q -y "-d$extracted" $installer @patterns
    if ($LASTEXITCODE -ne 0) { throw 'Pinned USB/IP installer extraction failed.' }
    $source = Join-Path $extracted '{tmp}'
    $hashes = [ordered]@{}
    foreach ($name in $UsbIpFiles) {
        $hashes[$name] = (Get-FileHash -LiteralPath (Join-Path $source $name) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    [ordered]@{
        version=$UsbIpVersion; tag=$UsbIpReleaseTag; architecture='x64'
        installer_sha256=$UsbIpInstallerHash; files=$hashes
    } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $source 'release-lock.json') -Encoding UTF8
    Assert-UsbIpPackage $source
    Assert-CatalogMembers $source $signtool
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    foreach ($name in $UsbIpFiles + @('release-lock.json')) {
        Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $OutDir $name) -Force
    }
    Write-Output 'Verified and staged original Microsoft-signed USB/IP transport 0.9.8.1 x64.'
} finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
