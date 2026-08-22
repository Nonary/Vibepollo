[CmdletBinding()]
param(
    [switch] $Uninstall,
    [switch] $InstallerBestEffort,
    [ValidateSet(0, 1)]
    [int] $AllowLocalTestCertificate = 0,
    [ValidateSet(0, 1)]
    [int] $RemoveDriverStorePackage = 0
)

$ErrorActionPreference = 'Stop'

$packageFiles = @(
    'driver/VibeshineVhfGamepad.inf',
    'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.cat',
    'tools/VibeshineVhfGamepadDeviceSetup.exe',
    'manifest.json',
    'release-lock.json'
)
$manifestPayload = @(
    'driver/VibeshineVhfGamepad.inf',
    'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.cat',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)
$localTestCertificate = 'driver/VibeshineVhfGamepad.cer'
$expectedProducerRepository = 'Nonary/libvirtualgamepad'
$expectedProducerTag = 'v0.1.0-beta.2'
$expectedProducerAsset = 'libvirtualgamepad-0.1.0-beta.2-windows-x64.zip'
$expectedProducerArchiveSha256 = '354c11239a91fd9fb2f52d45449de8409d96a4e4ed998f2794b5465eaec2434b'
$expectedProducerSourceRevision = '52cbb8f27cbeb18baec53ca5d7b88081f0787a06'
$expectedDriverVer = '08/21/2026,0.1.0.30'
$expectedProtocolVersion = 2
$expectedSignPathFoundationSignerSubject = 'CN=SignPath Foundation, O=SignPath Foundation, L=Lewes, S=Delaware, C=US'

# Signing channel for a package that ships unsigned and is signed by the
# consumer MSI signing request instead. SignPath is not available on
# Nonary/libvirtualgamepad, so its releases cannot carry a production
# signature; the catalogue is signed downstream, inside the MSI.
$msiRequestChannel = 'msi-request-signing'

# Under that channel these two files are re-signed after the manifest was
# written, so neither their hash nor their signer identity can be pinned by the
# upstream package. Everything else about them is still checked: the catalogue
# must carry an intact signature that Windows trusts, and the catalogue is what
# attests to VibeshineVhfGamepad.dll, whose hash IS still pinned.
$downstreamSignedFiles = @(
    'driver/VibeshineVhfGamepad.cat',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)
$rebootExitCode = 3010

function Write-DriverMessage {
    param([Parameter(Mandatory = $true)][string] $Message)
    Write-Host "[VibeshineVhfGamepad] $Message"
}

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script from an elevated Administrator session because it changes PnP devices and the Driver Store.'
    }
}

function Assert-File {
    param([Parameter(Mandatory = $true)][string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "[VibeshineVhfGamepad] Required package artifact is missing: $Path"
    }
    if ((Get-Item -LiteralPath $Path).Length -le 0) {
        throw "[VibeshineVhfGamepad] Required package artifact is empty: $Path"
    }
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string] $Path)

    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $hasher = [System.Security.Cryptography.SHA256]::Create()
        try {
            return ([System.BitConverter]::ToString($hasher.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
        } finally {
            $hasher.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
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
        throw "[VibeshineVhfGamepad] $Name does not match.`nExpected:`n$expectedText`nActual:`n$actualText"
    }
}

function Get-Thumbprint {
    param([Parameter(Mandatory = $true)] $Certificate)
    return $Certificate.Thumbprint.Replace(' ', '').ToUpperInvariant()
}

function Get-ValidatedSharedPublisherCertificate {
    param(
        [Parameter(Mandatory = $true)] $CatalogSignature,
        [Parameter(Mandatory = $true)] $ToolSignature,
        [Parameter(Mandatory = $true)][string] $ExpectedSubject
    )

    if ($null -eq $CatalogSignature.SignerCertificate -or
        $null -eq $ToolSignature.SignerCertificate -or
        $CatalogSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
        $ToolSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        throw '[VibeshineVhfGamepad] The downstream-signed catalog and setup tool must have valid Authenticode signatures before publisher trust is changed.'
    }
    $catalogSubject = [string] $CatalogSignature.SignerCertificate.Subject
    $toolSubject = [string] $ToolSignature.SignerCertificate.Subject
    if (-not [string]::Equals($catalogSubject, $ExpectedSubject, [System.StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals($toolSubject, $ExpectedSubject, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "[VibeshineVhfGamepad] Refusing publisher trust for an unexpected signer identity. Expected '$ExpectedSubject'; catalog='$catalogSubject'; setup='$toolSubject'."
    }
    $catalogThumbprint = Get-Thumbprint -Certificate $CatalogSignature.SignerCertificate
    $toolThumbprint = Get-Thumbprint -Certificate $ToolSignature.SignerCertificate
    if ($catalogThumbprint -ne $toolThumbprint) {
        throw '[VibeshineVhfGamepad] Catalog and root-device setup tool were signed by different certificates.'
    }
    return $CatalogSignature.SignerCertificate
}

function Ensure-ProductionPublisherTrusted {
    param(
        [Parameter(Mandatory = $true)] $PublisherCertificate,
        [Parameter(Mandatory = $true)][string] $ExpectedSubject
    )

    $publisherSubject = [string] $PublisherCertificate.Subject
    if (-not [string]::Equals($publisherSubject, $ExpectedSubject, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "[VibeshineVhfGamepad] Refusing to modify publisher trust for unexpected signer '$publisherSubject'."
    }
    $thumbprint = Get-Thumbprint -Certificate $PublisherCertificate
    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new('TrustedPublisher', 'LocalMachine')
    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $existing = $store.Certificates.Find(
            [System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,
            $thumbprint,
            $false)
        if ($existing.Count -eq 0) {
            $store.Add($PublisherCertificate)
        }
    } finally {
        $store.Close()
    }

    # Reopen independently and fail before PnPUtil if Windows did not persist
    # the exact signer validated on both downstream-signed package files.
    $verificationStore = [System.Security.Cryptography.X509Certificates.X509Store]::new('TrustedPublisher', 'LocalMachine')
    try {
        $verificationStore.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly)
        $verified = $verificationStore.Certificates.Find(
            [System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,
            $thumbprint,
            $false)
        if ($verified.Count -eq 0) {
            throw "[VibeshineVhfGamepad] Failed to establish publisher trust for validated signer $thumbprint."
        }
    } finally {
        $verificationStore.Close()
    }
    Write-DriverMessage "Validated publisher $thumbprint is present in LocalMachine\TrustedPublisher."
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

function Assert-ReleaseLock {
    param(
        [Parameter(Mandatory = $true)][string] $Root,
        [Parameter(Mandatory = $true)] $Manifest,
        [Parameter(Mandatory = $true)] $CatalogSignature,
        [Parameter(Mandatory = $true)] $ToolSignature
    )

    $lockPath = Join-Path $Root 'release-lock.json'
    Assert-File -Path $lockPath
    $lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
    if ($null -eq $lock -or $lock.schema_version -ne 2) {
        throw "[VibeshineVhfGamepad] Release lock is incomplete or unsupported: $lockPath"
    }

    $channel = Get-RequiredStringProperty -Object $lock -Name 'channel' -Context 'Release lock'
    $sourceRevision = Get-RequiredStringProperty -Object $lock -Name 'source_revision' -Context 'Release lock'
    $driverVer = Get-RequiredStringProperty -Object $lock -Name 'driver_ver' -Context 'Release lock'
    $signerIsPinnedUpstream = $Manifest.signing.channel -ne $msiRequestChannel
    if ($signerIsPinnedUpstream) {
        $catalogSigner = Get-RequiredStringProperty -Object $lock -Name 'catalog_signer_thumbprint' -Context 'Release lock'
        $deviceSetupSigner = Get-RequiredStringProperty -Object $lock -Name 'device_setup_signer_thumbprint' -Context 'Release lock'
        if ($catalogSigner -notmatch '^[0-9a-fA-F]{40}$' -or $deviceSetupSigner -notmatch '^[0-9a-fA-F]{40}$') {
            throw '[VibeshineVhfGamepad] Release lock has invalid signer thumbprints.'
        }
        if ($catalogSigner.ToUpperInvariant() -ne (Get-Thumbprint -Certificate $CatalogSignature.SignerCertificate) -or
            $deviceSetupSigner.ToUpperInvariant() -ne (Get-Thumbprint -Certificate $ToolSignature.SignerCertificate)) {
            throw '[VibeshineVhfGamepad] Release lock does not describe the signed VHF package.'
        }
    }
    if ($sourceRevision.ToLowerInvariant() -ne $Manifest.source_revision.ToLowerInvariant() -or
        $driverVer -ne $Manifest.driver_ver -or
        [uint16] $lock.protocol_version -ne [uint16] $Manifest.protocol_version -or
        $lock.manifest_sha256.ToLowerInvariant() -ne (Get-Sha256 -Path (Join-Path $Root 'manifest.json')) -or
        $lock.signing_channel -cne $Manifest.signing.channel) {
        throw '[VibeshineVhfGamepad] Release lock does not describe this VHF package.'
    }
    foreach ($entry in @($Manifest.files)) {
        $property = $lock.producer_payload_sha256.PSObject.Properties[[string] $entry.path]
        if ($null -eq $property -or [string] $property.Value -cne [string] $entry.sha256) {
            throw "[VibeshineVhfGamepad] Release lock payload hash mismatch for '$($entry.path)'."
        }
    }
    foreach ($name in @('install.ps1', 'cleanup.ps1')) {
        $property = $lock.installer_script_sha256.PSObject.Properties[$name]
        if ($null -eq $property -or [string] $property.Value -cne (Get-Sha256 -Path (Join-Path $Root $name))) {
            throw "[VibeshineVhfGamepad] Release lock installer-script hash mismatch for '$name'."
        }
    }

    if ($Manifest.signing.channel -eq 'self-signed-local-test') {
        if ($channel -ne 'self-signed-local-test') {
            throw '[VibeshineVhfGamepad] Local-test package has a non-local release lock.'
        }
        return
    }

    if ($Manifest.signing.channel -notin @('external-catalog-signing', $msiRequestChannel) -or
        $channel -ne 'production') {
        throw '[VibeshineVhfGamepad] Production package declares an unsupported signing channel.'
    }
    if ($lock.repository -cne $expectedProducerRepository -or
        $lock.release_tag -cne $expectedProducerTag -or
        $lock.release_asset_name -cne $expectedProducerAsset -or
        $lock.release_asset_sha256.ToLowerInvariant() -cne $expectedProducerArchiveSha256 -or
        $sourceRevision.ToLowerInvariant() -cne $expectedProducerSourceRevision -or
        $driverVer -cne $expectedDriverVer -or
        [uint16] $lock.protocol_version -ne $expectedProtocolVersion) {
        throw '[VibeshineVhfGamepad] Production release lock does not match the pinned producer release.'
    }
    if ($Manifest.signing.channel -eq $msiRequestChannel) {
        if ($lock.signing_channel -cne $msiRequestChannel -or
            @($lock.signed_downstream).Count -ne 2 -or
            (@($lock.signed_downstream) -join ',') -cne ($downstreamSignedFiles -join ',')) {
            throw '[VibeshineVhfGamepad] MSI-signing package has an incompatible downstream-signing lock.'
        }
    }
}

function Get-ManifestEntry {
    param(
        [Parameter(Mandatory = $true)] $Manifest,
        [Parameter(Mandatory = $true)][string] $RelativePath
    )

    $entry = @($Manifest.files | Where-Object { $_.path -eq $RelativePath })
    if ($entry.Count -ne 1 -or [string]::IsNullOrWhiteSpace($entry[0].sha256) -or
        $entry[0].sha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw "[VibeshineVhfGamepad] Manifest lacks one valid SHA-256 for '$RelativePath'."
    }
    return $entry[0]
}

function Assert-ManifestHash {
    param(
        [Parameter(Mandatory = $true)] $Manifest,
        [Parameter(Mandatory = $true)][string] $Root,
        [Parameter(Mandatory = $true)][string] $RelativePath
    )

    $entry = Get-ManifestEntry -Manifest $Manifest -RelativePath $RelativePath
    $path = Join-Path $Root ($RelativePath -replace '/', '\\')
    Assert-File -Path $path
    $actual = Get-Sha256 -Path $path
    if ($actual -ne $entry.sha256.ToLowerInvariant()) {
        throw "[VibeshineVhfGamepad] Manifest hash mismatch for '$RelativePath'."
    }
}

function Assert-ManifestPayloadContract {
    param(
        [Parameter(Mandatory = $true)] $Manifest,
        [Parameter(Mandatory = $true)][string] $Root
    )

    $expectedPayload = @($manifestPayload)
    if ([string] $Manifest.signing.channel -eq 'self-signed-local-test') {
        $expectedPayload += $localTestCertificate
    }
    Assert-ExactList `
        -Actual @($Manifest.files | ForEach-Object { [string] $_.path } | Sort-Object) `
        -Expected @($expectedPayload | Sort-Object) `
        -Name 'manifest file list'

    # manifest.json cannot hash itself: it is metadata written after the
    # immutable payload hashes are known. Hash-check every channel-owned file.
    $signedDownstream = [string] $Manifest.signing.channel -eq $msiRequestChannel
    foreach ($relativePath in $expectedPayload) {
        if ($signedDownstream -and $downstreamSignedFiles -contains $relativePath) {
            # Re-signed after this manifest was written, so the recorded hash
            # is expected not to match. Its signature is checked below.
            continue
        }
        Assert-ManifestHash -Manifest $Manifest -Root $Root -RelativePath $relativePath
    }
}

function Ensure-LocalTestCertificateTrusted {
    param(
        [Parameter(Mandatory = $true)][string] $CertificatePath,
        [Parameter(Mandatory = $true)] $CatalogSignature,
        [Parameter(Mandatory = $true)] $ToolSignature
    )

    $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($CertificatePath)
    $thumbprint = Get-Thumbprint -Certificate $certificate
    if ((Get-Thumbprint -Certificate $CatalogSignature.SignerCertificate) -ne $thumbprint -or
        (Get-Thumbprint -Certificate $ToolSignature.SignerCertificate) -ne $thumbprint) {
        throw '[VibeshineVhfGamepad] Local-test certificate does not match every signed package artifact.'
    }

    foreach ($store in @('Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher')) {
        $existing = @(Get-ChildItem -Path $store -ErrorAction Stop | Where-Object {
            $_.Thumbprint.Replace(' ', '').ToUpperInvariant() -eq $thumbprint
        })
        if ($existing.Count -eq 0) {
            Import-Certificate -FilePath $CertificatePath -CertStoreLocation $store | Out-Null
            Write-DriverMessage "Trusted the explicit local-test certificate in $store."
        }
    }
}

function Assert-DriverPackage {
    param([Parameter(Mandatory = $true)][string] $Root)

    foreach ($relativePath in $packageFiles) {
        Assert-File -Path (Join-Path $Root ($relativePath -replace '/', '\\'))
    }

    $manifestPath = Join-Path $Root 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($null -eq $manifest -or $manifest.schema_version -ne 1 -or $manifest.platform -ne 'x64' -or
        [string]::IsNullOrWhiteSpace($manifest.source_revision) -or
        [string]::IsNullOrWhiteSpace($manifest.driver_ver) -or
        [uint16] $manifest.protocol_version -eq 0 -or
        $null -eq $manifest.files -or $null -eq $manifest.signing) {
        throw "[VibeshineVhfGamepad] Manifest is incomplete or unsupported: $manifestPath"
    }
    $signedDownstream = $manifest.signing.channel -eq $msiRequestChannel
    if ($signedDownstream -and
        (@($manifest.signing.signed_downstream).Count -ne 2 -or
         (@($manifest.signing.signed_downstream) -join ',') -cne ($downstreamSignedFiles -join ','))) {
        throw '[VibeshineVhfGamepad] MSI-signing manifest does not declare exactly the catalog and setup tool.'
    }
    Assert-ManifestPayloadContract -Manifest $manifest -Root $Root

    $certificatePath = Join-Path $Root ($localTestCertificate -replace '/', '\\')
    $hasLocalCertificate = Test-Path -LiteralPath $certificatePath -PathType Leaf
    if ($hasLocalCertificate) {
        if ($AllowLocalTestCertificate -eq 0) {
            throw '[VibeshineVhfGamepad] Refusing a local-test package without -AllowLocalTestCertificate 1.'
        }
        if ($manifest.signing.channel -ne 'self-signed-local-test') {
            throw '[VibeshineVhfGamepad] A production manifest must not include a local-test certificate.'
        }
    } elseif ($manifest.signing.channel -eq 'self-signed-local-test') {
        throw '[VibeshineVhfGamepad] Local-test manifest is missing its public certificate.'
    }
    if ($hasLocalCertificate -and $manifest.signing.channel -eq $msiRequestChannel) {
        throw '[VibeshineVhfGamepad] An MSI-signed package must not carry a local-test certificate.'
    }
    if (-not $hasLocalCertificate -and
        ($manifest.source_revision.ToLowerInvariant() -cne $expectedProducerSourceRevision -or
         $manifest.driver_ver -cne $expectedDriverVer -or
         [uint16] $manifest.protocol_version -ne $expectedProtocolVersion)) {
        throw '[VibeshineVhfGamepad] Production manifest does not match the pinned producer source, DriverVer, or protocol.'
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
    $productionPublisherCertificate = $null
    if (-not $signedDownstream) {
        if ((Get-Thumbprint -Certificate $catalogSignature.SignerCertificate) -ne $manifest.signing.signer_thumbprint.ToUpperInvariant() -or
            (Get-Thumbprint -Certificate $toolSignature.SignerCertificate) -ne $manifest.signing.device_setup_signer_thumbprint.ToUpperInvariant()) {
            throw '[VibeshineVhfGamepad] Manifest signer identity does not match the signed package artifacts.'
        }
    } else {
        # The signed MSI authenticates these two package files. Bind silent PnP
        # trust to the exact shared SignPath Foundation signer certificate,
        # independent of any certificate installed by the optional display
        # driver.
        $productionPublisherCertificate = Get-ValidatedSharedPublisherCertificate `
            -CatalogSignature $catalogSignature `
            -ToolSignature $toolSignature `
            -ExpectedSubject $expectedSignPathFoundationSignerSubject
    }

    Assert-ReleaseLock -Root $Root -Manifest $manifest -CatalogSignature $catalogSignature -ToolSignature $toolSignature

    if ($hasLocalCertificate) {
        Ensure-LocalTestCertificateTrusted -CertificatePath $certificatePath -CatalogSignature $catalogSignature -ToolSignature $toolSignature
        $catalogSignature = Get-AuthenticodeSignature -LiteralPath $catalogPath
        $toolSignature = Get-AuthenticodeSignature -LiteralPath $toolPath
    }

    if ($catalogSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
        $toolSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        throw '[VibeshineVhfGamepad] Catalog or root-device setup tool is not trusted by this Windows installation.'
    }

    return [PSCustomObject]@{
        InfPath = Join-Path $Root 'driver/VibeshineVhfGamepad.inf'
        SetupToolPath = $toolPath
        ProductionPublisherCertificate = $productionPublisherCertificate
    }
}

function Resolve-SystemToolPath {
    param([Parameter(Mandatory = $true)][string] $ToolName)

    $systemRoot = if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) { 'C:\Windows' } else { $env:SystemRoot }
    foreach ($candidate in @(
        (Join-Path $systemRoot "Sysnative\$ToolName"),
        (Join-Path $systemRoot "System32\$ToolName"),
        (Join-Path $systemRoot "SysWOW64\$ToolName")
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    return Join-Path $systemRoot "System32\$ToolName"
}

function Invoke-PnpUtil {
    param(
        [Parameter(Mandatory = $true)][string[]] $Arguments,
        [int[]] $AllowedExitCodes = @(0, 3010)
    )

    $pnputil = Resolve-SystemToolPath -ToolName 'pnputil.exe'
    Assert-File -Path $pnputil
    # Keep PnPUtil's localized diagnostic text visible without allowing it to
    # become this function's return value.
    & $pnputil @Arguments | Out-Host
    $exitCode = $LASTEXITCODE
    if ($exitCode -notin $AllowedExitCodes) {
        throw "[VibeshineVhfGamepad] pnputil failed with exit code $exitCode."
    }
    return $exitCode
}

function Remove-DriverStorePackage {
    $command = Get-Command Get-WindowsDriver -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw '[VibeshineVhfGamepad] Get-WindowsDriver is unavailable; refusing to guess Driver Store package names.'
    }

    $packages = @(Get-WindowsDriver -Online -All | Where-Object {
        $_.OriginalFileName -eq 'VibeshineVhfGamepad.inf' -and -not [string]::IsNullOrWhiteSpace($_.PublishedName)
    })
    $rebootRequired = $false
    foreach ($package in $packages) {
        Write-DriverMessage "Removing owned Driver Store package $($package.PublishedName)."
        $exitCode = Invoke-PnpUtil -Arguments @('/delete-driver', $package.PublishedName, '/uninstall')
        $rebootRequired = $rebootRequired -or ($exitCode -eq $rebootExitCode)
    }
    return $rebootRequired
}

function Install-DriverPackage {
    Assert-Administrator
    $package = Assert-DriverPackage -Root $PSScriptRoot
    if ($null -ne $package.ProductionPublisherCertificate) {
        Ensure-ProductionPublisherTrusted `
            -PublisherCertificate $package.ProductionPublisherCertificate `
            -ExpectedSubject $expectedSignPathFoundationSignerSubject
    }
    # PnPUtil performs Windows' catalog-to-INF/DLL membership validation while
    # it stages the package. The installed product deliberately does not ship
    # SDK-only signtool.exe just to repeat that platform validation.
    $pnputilExitCode = Invoke-PnpUtil -Arguments @('/add-driver', $package.InfPath)
    & $package.SetupToolPath install --inf $package.InfPath | Out-Host
    $setupExitCode = $LASTEXITCODE
    if ($setupExitCode -ne 0 -and $setupExitCode -ne $rebootExitCode) {
        throw "[VibeshineVhfGamepad] Root-device setup failed with exit code $setupExitCode."
    }
    # The setup tool's zero exit code is a stronger result than PnPUtil's
    # advisory 3010: it has reloaded the exact owned source node, opened its
    # private interface, completed the versioned query handshake, and (when
    # PnP requested a restart) attested the selected DriverVer. Do not request
    # a reboot that Windows no longer needs just because the initial Driver
    # Store staging step asked for one.
    if ($setupExitCode -eq 0) {
        if ($pnputilExitCode -eq $rebootExitCode) {
            Write-DriverMessage 'Windows requested restart while staging the package, but the VHF source device is live now.'
        }
        Write-DriverMessage 'Installed and verified the VHF source device.'
        return 0
    }
    if ($pnputilExitCode -eq $rebootExitCode -or $setupExitCode -eq $rebootExitCode) {
        Write-DriverMessage 'VIRTUAL_GAMEPAD_RESTART_REQUIRED'
        return $rebootExitCode
    }
    throw "[VibeshineVhfGamepad] Root-device setup ended with unexpected exit code $setupExitCode."
}

function Uninstall-DriverPackage {
    Assert-Administrator
    $package = Assert-DriverPackage -Root $PSScriptRoot
    & $package.SetupToolPath remove | Out-Host
    $setupExitCode = $LASTEXITCODE
    if ($setupExitCode -ne 0 -and $setupExitCode -ne $rebootExitCode) {
        throw "[VibeshineVhfGamepad] Root-device removal failed with exit code $setupExitCode."
    }
    $driverStoreRebootRequired = $false
    if ($RemoveDriverStorePackage -ne 0) {
        $driverStoreRebootRequired = Remove-DriverStorePackage
    }
    if ($setupExitCode -eq $rebootExitCode -or $driverStoreRebootRequired) {
        Write-DriverMessage 'VIRTUAL_GAMEPAD_RESTART_REQUIRED'
        return $rebootExitCode
    }
    Write-DriverMessage 'Removed only Vibeshine-owned VHF source devices.'
    return 0
}

try {
    $exitCode = if ($Uninstall) { Uninstall-DriverPackage } else { Install-DriverPackage }
    if ($exitCode -eq $rebootExitCode -and $InstallerBestEffort) {
        exit 0
    }
    exit $exitCode
} catch {
    $message = $_.Exception.Message
    if ($InstallerBestEffort) {
        Write-DriverMessage "VIRTUAL_GAMEPAD_DRIVER_WARNING: $message"
        exit 0
    }
    Write-Error "[VibeshineVhfGamepad] $message" -ErrorAction Continue
    exit 1
}
