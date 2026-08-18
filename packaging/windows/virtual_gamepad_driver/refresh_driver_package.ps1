[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $PrebuiltPackageDir,

    [Parameter(Mandatory = $true)]
    [string] $PackageDir,

    [switch] $ValidateOnly,
    [switch] $AllowLocalTestPackage,
    [string] $ReleaseTag,
    [string] $ExpectedReleaseAssetSha256,
    [string] $ExpectedCatalogSignerThumbprint,
    [string] $ExpectedDeviceSetupSignerThumbprint,
    [string] $SignToolPath
)

$ErrorActionPreference = 'Stop'

$packageFiles = @(
    'driver/VibeshineVhfGamepad.inf',
    'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.cat',
    'tools/VibeshineVhfGamepadDeviceSetup.exe',
    'manifest.json'
)
$manifestPayload = @(
    'driver/VibeshineVhfGamepad.inf',
    'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.cat',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)
$localTestCertificate = 'driver/VibeshineVhfGamepad.cer'
$releaseLockFile = 'release-lock.json'

function Normalize-Hex {
    param(
        [Parameter(Mandatory = $true)][string] $Value,
        [Parameter(Mandatory = $true)][int] $Length,
        [Parameter(Mandatory = $true)][string] $Name
    )

    if ($Value -notmatch "^[0-9a-fA-F]{$Length}$") {
        throw "[VibeshineVhfGamepad] $Name must be a $Length-character hexadecimal value."
    }
    return $Value.ToUpperInvariant()
}

if (-not $AllowLocalTestPackage) {
    if ([string]::IsNullOrWhiteSpace($ReleaseTag)) {
        throw '[VibeshineVhfGamepad] Production package refresh requires -ReleaseTag.'
    }
    $ExpectedReleaseAssetSha256 = Normalize-Hex -Value $ExpectedReleaseAssetSha256 -Length 64 -Name 'ExpectedReleaseAssetSha256'
    $ExpectedCatalogSignerThumbprint = Normalize-Hex -Value $ExpectedCatalogSignerThumbprint -Length 40 -Name 'ExpectedCatalogSignerThumbprint'
    $ExpectedDeviceSetupSignerThumbprint = Normalize-Hex -Value $ExpectedDeviceSetupSignerThumbprint -Length 40 -Name 'ExpectedDeviceSetupSignerThumbprint'
}

function Write-Step {
    param([Parameter(Mandatory = $true)][string] $Message)
    Write-Host "[VibeshineVhfGamepad] $Message"
}

function Resolve-RequiredPath {
    param(
        [Parameter(Mandatory = $true)][string] $Path,
        [switch] $Leaf
    )

    if ($Leaf) {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            throw "[VibeshineVhfGamepad] Required file is missing: $Path"
        }
    } elseif (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "[VibeshineVhfGamepad] Required directory is missing: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Assert-File {
    param([Parameter(Mandatory = $true)][string] $Path)

    Resolve-RequiredPath -Path $Path -Leaf | Out-Null
    if ((Get-Item -LiteralPath $Path).Length -le 0) {
        throw "[VibeshineVhfGamepad] Required package artifact is empty: $Path"
    }
}

function Resolve-SignTool {
    param([string] $ExplicitPath)

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        return Resolve-RequiredPath -Path $ExplicitPath -Leaf
    }

    foreach ($root in @(
        'D:\Software\WinSDK\bin',
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    )) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) {
            continue
        }
        foreach ($version in @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)) {
            foreach ($architecture in @('x64', 'x86')) {
                $candidate = Join-Path $version.FullName "$architecture\signtool.exe"
                if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                    return (Resolve-Path -LiteralPath $candidate).Path
                }
            }
        }
        foreach ($architecture in @('x64', 'x86')) {
            $candidate = Join-Path $root "$architecture\signtool.exe"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
        }
    }

    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw '[VibeshineVhfGamepad] signtool.exe is required to verify the driver package.'
    }
    return $command.Source
}

function Resolve-PrebuiltPackageRoot {
    param([Parameter(Mandatory = $true)][string] $Path)

    $resolved = Resolve-RequiredPath -Path $Path
    $directManifest = Join-Path $resolved 'manifest.json'
    if (Test-Path -LiteralPath $directManifest -PathType Leaf) {
        return $resolved
    }

    foreach ($candidate in @(Get-ChildItem -LiteralPath $resolved -Recurse -File -Filter manifest.json -ErrorAction SilentlyContinue)) {
        $candidateRoot = Split-Path -Parent $candidate.FullName
        if (Test-Path -LiteralPath (Join-Path $candidateRoot 'driver/VibeshineVhfGamepad.inf') -PathType Leaf) {
            return $candidateRoot
        }
    }
    throw "[VibeshineVhfGamepad] Unable to locate a libvirtualgamepad package under $resolved"
}

function Get-Thumbprint {
    param([Parameter(Mandatory = $true)] $Certificate)
    return $Certificate.Thumbprint.Replace(' ', '').ToUpperInvariant()
}

function Get-RequiredStringProperty {
    param(
        [Parameter(Mandatory = $true)] $Object,
        [Parameter(Mandatory = $true)][string] $Name,
        [Parameter(Mandatory = $true)][string] $Context
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or [string]::IsNullOrWhiteSpace([string] $property.Value)) {
        throw "[VibeshineVhfGamepad] $Context lacks required '$Name'."
    }
    return [string] $property.Value
}

function Assert-ManifestHash {
    param(
        [Parameter(Mandatory = $true)] $Manifest,
        [Parameter(Mandatory = $true)][string] $Root,
        [Parameter(Mandatory = $true)][string] $RelativePath
    )

    $entry = @($Manifest.files | Where-Object { $_.path -eq $RelativePath })
    if ($entry.Count -ne 1 -or [string]::IsNullOrWhiteSpace($entry[0].sha256) -or
        $entry[0].sha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw "[VibeshineVhfGamepad] Manifest lacks one valid SHA-256 for '$RelativePath'."
    }
    $path = Join-Path $Root ($RelativePath -replace '/', '\\')
    Assert-File -Path $path
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $entry[0].sha256.ToLowerInvariant()) {
        throw "[VibeshineVhfGamepad] Manifest hash mismatch for '$RelativePath'."
    }
}

function Assert-Package {
    param(
        [Parameter(Mandatory = $true)][string] $Root,
        [switch] $AllowLocalTest
    )

    foreach ($relativePath in $packageFiles) {
        Assert-File -Path (Join-Path $Root ($relativePath -replace '/', '\\'))
    }

    $manifestPath = Join-Path $Root 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($null -eq $manifest -or $manifest.schema_version -ne 1 -or
        [string]::IsNullOrWhiteSpace($manifest.source_revision) -or
        [string]::IsNullOrWhiteSpace($manifest.driver_ver) -or
        $manifest.platform -ne 'x64' -or $null -eq $manifest.files -or $null -eq $manifest.signing) {
        throw "[VibeshineVhfGamepad] Manifest is incomplete or uses an unsupported schema: $manifestPath"
    }

    # manifest.json is metadata written after the payload hashes are known; a
    # self-hash would be circular. Require it to exist and parse, but only
    # hash-check the immutable artifacts it describes.
    foreach ($relativePath in $manifestPayload) {
        Assert-ManifestHash -Manifest $manifest -Root $Root -RelativePath $relativePath
    }

    $certificatePath = Join-Path $Root ($localTestCertificate -replace '/', '\\')
    $hasLocalCertificate = Test-Path -LiteralPath $certificatePath -PathType Leaf
    if ($hasLocalCertificate -and -not $AllowLocalTest) {
        throw '[VibeshineVhfGamepad] Refusing a self-signed local-test package without -AllowLocalTestPackage.'
    }
    if (-not $hasLocalCertificate -and $manifest.signing.channel -eq 'self-signed-local-test') {
        throw '[VibeshineVhfGamepad] A manifest declares local-test signing but the public certificate is absent.'
    }
    if ($hasLocalCertificate -and $manifest.signing.channel -ne 'self-signed-local-test') {
        throw '[VibeshineVhfGamepad] A production manifest must not include a local-test certificate.'
    }

    $catalogPath = Join-Path $Root 'driver/VibeshineVhfGamepad.cat'
    $toolPath = Join-Path $Root 'tools/VibeshineVhfGamepadDeviceSetup.exe'
    $catalogSignature = Get-AuthenticodeSignature -LiteralPath $catalogPath
    $toolSignature = Get-AuthenticodeSignature -LiteralPath $toolPath
    if ($null -eq $catalogSignature.SignerCertificate -or $null -eq $toolSignature.SignerCertificate -or
        $catalogSignature.Status -eq [System.Management.Automation.SignatureStatus]::HashMismatch -or
        $toolSignature.Status -eq [System.Management.Automation.SignatureStatus]::HashMismatch) {
        throw '[VibeshineVhfGamepad] Catalog or root-device setup tool has no intact Authenticode signature.'
    }
    if ((Get-Thumbprint -Certificate $catalogSignature.SignerCertificate) -ne $manifest.signing.signer_thumbprint.ToUpperInvariant() -or
        (Get-Thumbprint -Certificate $toolSignature.SignerCertificate) -ne $manifest.signing.device_setup_signer_thumbprint.ToUpperInvariant()) {
        throw '[VibeshineVhfGamepad] Manifest signer identity does not match the signed package artifacts.'
    }

    if (-not $AllowLocalTest) {
        if ($manifest.signing.channel -ne 'external-catalog-signing') {
            throw '[VibeshineVhfGamepad] Production package does not declare external catalog signing.'
        }
        if ((Get-Thumbprint -Certificate $catalogSignature.SignerCertificate) -ne $ExpectedCatalogSignerThumbprint -or
            (Get-Thumbprint -Certificate $toolSignature.SignerCertificate) -ne $ExpectedDeviceSetupSignerThumbprint) {
            throw '[VibeshineVhfGamepad] Signed package artifacts do not match the signer identities pinned by Vibeshine.'
        }
    }

    if ($hasLocalCertificate) {
        Assert-ManifestHash -Manifest $manifest -Root $Root -RelativePath $localTestCertificate
        $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
        $certificateThumbprint = Get-Thumbprint -Certificate $certificate
        if ((Get-Thumbprint -Certificate $catalogSignature.SignerCertificate) -ne $certificateThumbprint -or
            (Get-Thumbprint -Certificate $toolSignature.SignerCertificate) -ne $certificateThumbprint) {
            throw '[VibeshineVhfGamepad] Local-test certificate does not match every package signer.'
        }
        return $manifest
    }

    $signTool = Resolve-SignTool -ExplicitPath $SignToolPath
    & $signTool verify '/v' '/pa' $catalogPath | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "[VibeshineVhfGamepad] Catalog signature verification failed with exit code $LASTEXITCODE."
    }
    foreach ($payload in @(
        (Join-Path $Root 'driver/VibeshineVhfGamepad.inf'),
        (Join-Path $Root 'driver/VibeshineVhfGamepad.dll')
    )) {
        & $signTool verify '/v' '/pa' '/c' $catalogPath $payload | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "[VibeshineVhfGamepad] Catalog membership verification failed for '$payload' (exit code $LASTEXITCODE)."
        }
    }
    & $signTool verify '/v' '/pa' $toolPath | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "[VibeshineVhfGamepad] Root-device setup tool signature verification failed with exit code $LASTEXITCODE."
    }
    return $manifest
}

function Write-ReleaseLock {
    param(
        [Parameter(Mandatory = $true)][string] $Root,
        [Parameter(Mandatory = $true)] $Manifest,
        [switch] $AllowLocalTest
    )

    $lock = [ordered]@{
        schema_version = 1
        channel = if ($AllowLocalTest) { 'self-signed-local-test' } else { 'production' }
        release_tag = if ($AllowLocalTest -and [string]::IsNullOrWhiteSpace($ReleaseTag)) { 'local' } else { $ReleaseTag }
        release_asset_sha256 = if ($AllowLocalTest) { '' } else { $ExpectedReleaseAssetSha256.ToLowerInvariant() }
        catalog_signer_thumbprint = $Manifest.signing.signer_thumbprint.ToUpperInvariant()
        device_setup_signer_thumbprint = $Manifest.signing.device_setup_signer_thumbprint.ToUpperInvariant()
        source_revision = $Manifest.source_revision.ToLowerInvariant()
        driver_ver = $Manifest.driver_ver
    }
    $path = Join-Path $Root $releaseLockFile
    [System.IO.File]::WriteAllText(
        $path,
        ($lock | ConvertTo-Json -Depth 3),
        [System.Text.UTF8Encoding]::new($false))
}

function Assert-ReleaseLock {
    param(
        [Parameter(Mandatory = $true)][string] $Root,
        [Parameter(Mandatory = $true)] $Manifest,
        [switch] $AllowLocalTest
    )

    $path = Join-Path $Root $releaseLockFile
    Assert-File -Path $path
    $lock = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ($null -eq $lock -or $lock.schema_version -ne 1) {
        throw "[VibeshineVhfGamepad] Release lock is incomplete or unsupported: $path"
    }

    $lockChannel = Get-RequiredStringProperty -Object $lock -Name 'channel' -Context 'Release lock'
    $lockSourceRevision = Get-RequiredStringProperty -Object $lock -Name 'source_revision' -Context 'Release lock'
    $lockDriverVer = Get-RequiredStringProperty -Object $lock -Name 'driver_ver' -Context 'Release lock'
    $lockCatalogSigner = Normalize-Hex -Value (Get-RequiredStringProperty -Object $lock -Name 'catalog_signer_thumbprint' -Context 'Release lock') -Length 40 -Name 'release-lock catalog signer thumbprint'
    $lockDeviceSetupSigner = Normalize-Hex -Value (Get-RequiredStringProperty -Object $lock -Name 'device_setup_signer_thumbprint' -Context 'Release lock') -Length 40 -Name 'release-lock device-setup signer thumbprint'
    if ($lockSourceRevision.ToLowerInvariant() -ne $Manifest.source_revision.ToLowerInvariant() -or
        $lockDriverVer -ne $Manifest.driver_ver -or
        $lockCatalogSigner -ne $Manifest.signing.signer_thumbprint.ToUpperInvariant() -or
        $lockDeviceSetupSigner -ne $Manifest.signing.device_setup_signer_thumbprint.ToUpperInvariant()) {
        throw '[VibeshineVhfGamepad] Release lock does not describe the signed driver manifest.'
    }

    if ($AllowLocalTest) {
        if ($lockChannel -ne 'self-signed-local-test') {
            throw '[VibeshineVhfGamepad] Local-test package has a non-local release lock.'
        }
        return
    }

    if ($lockChannel -ne 'production' -or
        (Get-RequiredStringProperty -Object $lock -Name 'release_tag' -Context 'Release lock') -ne $ReleaseTag -or
        (Normalize-Hex -Value (Get-RequiredStringProperty -Object $lock -Name 'release_asset_sha256' -Context 'Release lock') -Length 64 -Name 'release-lock archive SHA-256') -ne $ExpectedReleaseAssetSha256 -or
        $lockCatalogSigner -ne $ExpectedCatalogSignerThumbprint -or
        $lockDeviceSetupSigner -ne $ExpectedDeviceSetupSignerThumbprint) {
        throw '[VibeshineVhfGamepad] Release lock does not match the Vibeshine-pinned production package.'
    }
}

$PackageDir = [System.IO.Path]::GetFullPath($PackageDir)
$PrebuiltPackageDir = Resolve-PrebuiltPackageRoot -Path $PrebuiltPackageDir
if ($PackageDir.TrimEnd('\', '/') -eq $PrebuiltPackageDir.TrimEnd('\', '/')) {
    throw '[VibeshineVhfGamepad] PackageDir and PrebuiltPackageDir must be different directories.'
}

$installerScripts = @(
    (Join-Path $PackageDir 'install.ps1'),
    (Join-Path $PackageDir 'cleanup.ps1')
)
foreach ($installerScript in $installerScripts) {
    Assert-File -Path $installerScript
}
$prebuiltManifest = Assert-Package -Root $PrebuiltPackageDir -AllowLocalTest:$AllowLocalTestPackage

if ($ValidateOnly) {
    $stagedManifest = Assert-Package -Root $PackageDir -AllowLocalTest:$AllowLocalTestPackage
    Assert-ReleaseLock -Root $PackageDir -Manifest $stagedManifest -AllowLocalTest:$AllowLocalTestPackage
    Write-Step "Validated staged VHF gamepad package at $PackageDir"
    exit 0
}

$stageDir = "$PackageDir.partial-$([System.Guid]::NewGuid().ToString('N'))"
$backupDir = "$PackageDir.previous-$([System.Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

try {
    foreach ($installerScript in $installerScripts) {
        Copy-Item -LiteralPath $installerScript -Destination (Join-Path $stageDir (Split-Path -Leaf $installerScript)) -Force
    }
    foreach ($relativePath in $packageFiles) {
        $source = Join-Path $PrebuiltPackageDir ($relativePath -replace '/', '\\')
        $destination = Join-Path $stageDir ($relativePath -replace '/', '\\')
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
    if ($AllowLocalTestPackage -and (Test-Path -LiteralPath (Join-Path $PrebuiltPackageDir ($localTestCertificate -replace '/', '\\')) -PathType Leaf)) {
        $destination = Join-Path $stageDir ($localTestCertificate -replace '/', '\\')
        Copy-Item -LiteralPath (Join-Path $PrebuiltPackageDir ($localTestCertificate -replace '/', '\\')) -Destination $destination -Force
    }
    $stagedManifest = Assert-Package -Root $stageDir -AllowLocalTest:$AllowLocalTestPackage
    Write-ReleaseLock -Root $stageDir -Manifest $stagedManifest -AllowLocalTest:$AllowLocalTestPackage
    Assert-ReleaseLock -Root $stageDir -Manifest $stagedManifest -AllowLocalTest:$AllowLocalTestPackage

    Move-Item -LiteralPath $PackageDir -Destination $backupDir
    try {
        Move-Item -LiteralPath $stageDir -Destination $PackageDir
    } catch {
        if ((Test-Path -LiteralPath $backupDir) -and -not (Test-Path -LiteralPath $PackageDir)) {
            Move-Item -LiteralPath $backupDir -Destination $PackageDir
        }
        throw
    }
    if (Test-Path -LiteralPath $backupDir) {
        Remove-Item -LiteralPath $backupDir -Recurse -Force
    }
    Write-Step "Refreshed VHF gamepad package assets from $PrebuiltPackageDir"
} finally {
    if (Test-Path -LiteralPath $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
