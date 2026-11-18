[CmdletBinding()]
param(
    [switch]$FunctionsOnly,
    [switch] $Uninstall,
    [ValidateSet(0, 1)]
    [int]$DailyUnsignedPolicy = 0,
    [switch]$InstallerBestEffort,
    [ValidateSet(0, 1)]
    [int] $AllowLocalTestCertificate = 0,
    [ValidateSet(0, 1)]
    [int] $AllowLocalGamepadCertificateTrust = 0,
    [string] $ConsentCertificateThumbprint = '',
    [ValidateSet(0, 1)]
    [int] $RemoveDriverStorePackage = 0
)

$ErrorActionPreference = 'Stop'

# DRIVER_INTEROP_COMPILER_BEGIN
# Windows PowerShell 5's Add-Type uses CodeDOM's ANSI environment block and
# rejects otherwise valid environments above 65535 bytes (#610). Invoke the
# same framework compiler directly so Windows inherits the full environment.
# Keep this helper inline: the driver scripts also ship as standalone files.
function Add-DriverInteropType {
    param(
        [Parameter(Mandatory = $true)][string]$TypeDefinition,
        [string[]]$ReferencedAssemblies = @('System', 'System.Core')
    )
    # PowerShell Core compiles in-process and has no Framework csc.exe.
    if ($PSVersionTable.PSEdition -eq 'Core') {
        Add-Type @PSBoundParameters
        return
    }
    $runtime = [Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()
    $compiler = Join-Path $runtime 'csc.exe'
    $temporary = Join-Path ([IO.Path]::GetTempPath()) ('driver-interop-' + [Guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $temporary -ErrorAction Stop
    try {
        $source = Join-Path $temporary 'interop.cs'
        $assembly = Join-Path $temporary 'interop.dll'
        [IO.File]::WriteAllText($source, $TypeDefinition, [Text.UTF8Encoding]::new($true))
        $compilerArguments = @('/nologo', '/noconfig', '/target:library', "/out:$assembly")
        foreach ($reference in $ReferencedAssemblies) {
            $compilerArguments += '/reference:' + (Join-Path $runtime ($reference + '.dll'))
        }
        $compilerArguments += $source
        $diagnostics = & $compiler @compilerArguments 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "Driver interop compilation failed (exit $LASTEXITCODE): $($diagnostics -join [Environment]::NewLine)"
        }
        # Load bytes so no assembly file remains locked when the scratch is removed.
        $null = [Reflection.Assembly]::Load([IO.File]::ReadAllBytes($assembly))
    } finally {
        Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction Stop
    }
}
# DRIVER_INTEROP_COMPILER_END


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
$expectedProducerTag = 'v0.1.0-beta.10'
$expectedProducerAsset = 'libvirtualgamepad-0.1.0-beta.10-windows-x64.zip'
$expectedProducerArchiveSha256 = '650cf8281d95d1fb9388f8117cadeab195727e3dcb4105607df45080b15d312c'
$expectedProducerSourceRevision = '5a3e4d222dd0b87556d4c564eb58b450ab579fb0'
$expectedDriverVer = '10/29/2026,0.1.0.92'
$expectedProtocolVersion = 2
$expectedSignPathFoundationSignerSubject = 'CN=SignPath Foundation, O=SignPath Foundation, L=Lewes, S=Delaware, C=US'

# The producer package ships unsigned for downstream consumer MSI signing.
# The catalogue is signed inside the consumer MSI signing request.
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

# DRIVER_INSTALL_SAFETY_BEGIN
# Kept inline so pinned producer packages do not need a new runtime file.
function ConvertTo-DriverComponentVersion {
    param([string]$Value)
    if ($Value -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw "Unknown driver component version: '$Value'. Driver unchanged." }
    foreach ($part in $Value.Split('.')) {
        if ([uint64]$part -gt 65535) { throw "Invalid driver component version: '$Value'. Driver unchanged." }
    }
    return [version]$Value
}

function Get-InfComponentVersion {
    param([Parameter(Mandatory = $true)][string]$Path)
    $inVersion = $false
    $versions = @()
    foreach ($line in (Get-Content -LiteralPath $Path -ErrorAction Stop)) {
        $text = ($line -split ';', 2)[0].Trim()
        if ($text -match '^\[(.+)\]$') { $inVersion = $Matches[1] -ieq 'Version'; continue }
        if ($inVersion -and $text -match '^DriverVer\s*=\s*[^,]+,\s*(\S+)\s*$') {
            $versions += ConvertTo-DriverComponentVersion $Matches[1]
        }
    }
    if ($versions.Count -ne 1) { throw "Missing or ambiguous INF DriverVer: $Path. Driver unchanged." }
    return $versions[0]
}

function Test-SafeDriverInstall {
    param(
        [Parameter(Mandatory = $true)][string]$InfPath,
        [Parameter(Mandatory = $true)][string]$CatalogPath,
        [Parameter(Mandatory = $true)][string]$HardwareId,
        [string]$WarningMarker = 'VIRTUAL_DISPLAY_DRIVER_WARNING',
        [bool]$LocalTestPackage = $false,
        [bool]$AllowUntrustedLocalPackage = $false
    )
    $target = Get-InfComponentVersion $InfPath
    $infName = [IO.Path]::GetFileName($InfPath)
    # Both queries must succeed. An empty successful query means missing;
    # errors, missing active versions, and unreadable store INFs mean unknown.
    if (Get-Command Get-SharedDriverInventory -ErrorAction SilentlyContinue) {
        $inventory = Get-SharedDriverInventory -InfName $infName -HardwareId $HardwareId
        $packages = @($inventory.Packages); $devices = @($inventory.Devices)
    } else {
    $packages = @(Get-WindowsDriver -Online -ErrorAction Stop | Where-Object {
        [IO.Path]::GetFileName([string]$_.OriginalFileName) -ieq $infName
    })
    $devices = @(Get-CimInstance -ClassName Win32_PnPSignedDriver -ErrorAction Stop | Where-Object {
        ([string]$_.DeviceID).StartsWith($HardwareId, [StringComparison]::OrdinalIgnoreCase) -or
        ([string]$_.HardwareID).Equals($HardwareId, [StringComparison]::OrdinalIgnoreCase)
    })
    }
    $installed = @()
    foreach ($device in $devices) {
        # CIM reports the active INF's numeric DriverVersion (not DriverDate).
        $installed += ConvertTo-DriverComponentVersion ([string]$device.DriverVersion)
    }
    foreach ($package in $packages) {
        $path = [string]$package.OriginalFileName
        if (-not [IO.Path]::IsPathRooted($path)) {
            if ([string]$package.Driver -notmatch '^oem\d+\.inf$') {
                throw 'Unknown published INF identity. Driver unchanged.'
            }
            $path = Join-Path (Join-Path $env:windir 'INF') ([string]$package.Driver)
        }
        $installed += Get-InfComponentVersion $path
    }
    if (@($installed | Where-Object { $_ -ge $target }).Count -gt 0) {
        Write-Host "Driver component $target skipped: an equal or newer active/Driver Store component exists; installed driver retained."
        return $false
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $CatalogPath -ErrorAction Stop
    $signer = $signature.SignerCertificate
    if ($null -eq $signer -or ($signature.Status -ne 'Valid' -and
        (-not ($LocalTestPackage -and $AllowUntrustedLocalPackage -and
            [string]$signature.Status -in @('NotTrusted', 'UnknownError'))))) {
        throw "$WarningMarker`: Driver component $target is missing or newer, but its catalog is unsigned or not trusted by Windows. Installed driver retained. Catalog signature status: $($signature.Status); detail: $($signature.StatusMessage). Windows requires deliberate trust/security setup before this local driver can be used; manual security review is required. Self-signed artifacts require existing Windows trust or independent validation and informed exact-certificate recipient consent; platform security policy may still reject the driver. This installer changes local certificate trust only after independent package validation and exact-certificate recipient consent. It never changes Secure Boot or enables test signing."
    }
    return $true
}
# DRIVER_INSTALL_SAFETY_END

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
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]] $Actual,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]] $Expected,
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

# A mapper package is identified explicitly, so old pinned gamepad-only archives
# retain their existing contract. New producer payloads must carry all three files.
function Test-MouseMapperDeclared {
    param([Parameter(Mandatory=$true)]$Manifest)
    return $null -ne $Manifest.PSObject.Properties['features'] -and
        $null -ne $Manifest.features -and $null -ne $Manifest.features.PSObject.Properties['mouse_mapper']
}
function Test-MouseMapperFeature {
    param([Parameter(Mandatory = $true)] $Manifest)
    if ($null -eq $Manifest.PSObject.Properties['features'] -or
        $null -eq $Manifest.features.PSObject.Properties['mouse_mapper']) { return $false }
    if ($Manifest.features.mouse_mapper -ne 1 -or $null -eq $Manifest.mouse_mapper -or
        $Manifest.mouse_mapper.transport_version -ne 1) { throw 'Unsupported mouse mapper package feature/version.' }
    return $true
}
function Get-MouseMapperPayload {
    return @('mouse_mapper/VibeshineVhfMouseMapper.inf', 'mouse_mapper/VibeshineVhfMouseMapper.sys', 'mouse_mapper/VibeshineVhfMouseMapper.cat')
}

function Assert-ManifestPayloadContract {
    param(
        [Parameter(Mandatory = $true)] $Manifest,
        [Parameter(Mandatory = $true)][string] $Root,
        [switch]$DeferMapper
    )

    $expectedPayload = @($manifestPayload)
    $hasMapper = if ($DeferMapper) { Test-MouseMapperDeclared -Manifest $Manifest } else { Test-MouseMapperFeature -Manifest $Manifest }
    if ($hasMapper) { $expectedPayload += @(Get-MouseMapperPayload) }
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
        if ($DeferMapper -and $relativePath -in @(Get-MouseMapperPayload)) { continue }
        if ($signedDownstream -and $downstreamSignedFiles -contains $relativePath) {
            # Re-signed after this manifest was written, so the recorded hash
            # is expected not to match. Its signature is checked below.
            continue
        }
        Assert-ManifestHash -Manifest $Manifest -Root $Root -RelativePath $relativePath
    }
}

function Assert-LocalTestCertificateIdentity {
    param(
        [Parameter(Mandatory = $true)][string] $CertificatePath,
        [Parameter(Mandatory = $true)] $CatalogSignature,
        [Parameter(Mandatory = $true)] $ToolSignature
    )

    $certificate = Get-PublicLocalGamepadCertificate -CertificatePath $CertificatePath
    $thumbprint = Get-Thumbprint -Certificate $certificate
    if ((Get-Thumbprint -Certificate $CatalogSignature.SignerCertificate) -ne $thumbprint -or
        (Get-Thumbprint -Certificate $ToolSignature.SignerCertificate) -ne $thumbprint) {
        throw '[VibeshineVhfGamepad] Local-test certificate does not match every signed package artifact.'
    }
    return $certificate
}

function Initialize-LocalGamepadTrustValidation {
    if ('VhfLocalTrustValidation' -as [type]) { return }
    $references = @('System', 'System.Core', 'System.Security')
    if ($PSVersionTable.PSEdition -eq 'Core') {
        $references += @('System.Runtime', 'System.Collections')
        $references += [System.Security.Cryptography.Pkcs.SignedCms].Assembly.Location
        $references += [System.Security.Cryptography.X509Certificates.X509Certificate2].Assembly.Location
    }
    Add-DriverInteropType -ReferencedAssemblies $references -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography.Pkcs;
public static class VhfLocalTrustValidation {
    [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int kind, ref CodeIntegrity info, uint size, IntPtr returned);
    [StructLayout(LayoutKind.Sequential)] struct CodeIntegrity { public uint Length, Options; }
    public static bool IsTestSigningEnabled() {
        var info=new CodeIntegrity { Length=8 };
        if(NtQuerySystemInformation(103,ref info,8,IntPtr.Zero)!=0) throw new InvalidOperationException("Cannot determine active Windows code-integrity policy.");
        return (info.Options & 2)!=0;
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct FileInfo {
        public uint Size; [MarshalAs(UnmanagedType.LPWStr)] public string Path; public IntPtr File, Subject;
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct TrustData {
        public uint Size; public IntPtr Callback, Sip; public uint UI, Revocation, Choice;
        public IntPtr File; public uint StateAction; public IntPtr StateData;
        public IntPtr Url; public uint Flags, Context; public IntPtr SignatureSettings;
    }
    [DllImport("wintrust.dll", ExactSpelling=true, CharSet=CharSet.Unicode)]
    static extern int WinVerifyTrust(IntPtr window, ref Guid action, ref TrustData data);
    [DllImport("wintrust.dll", ExactSpelling=true, CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CryptCATOpen(string path, uint flags, IntPtr provider, uint version, uint encoding);
    [DllImport("wintrust.dll", SetLastError=true)] static extern bool CryptCATClose(IntPtr catalog);
    [DllImport("wintrust.dll", SetLastError=true)] static extern IntPtr CryptCATEnumerateMember(IntPtr catalog, IntPtr previous);
    [DllImport("wintrust.dll", ExactSpelling=true, CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool CryptCATAdminAcquireContext2(out IntPtr context, IntPtr subsystem, string algorithm, IntPtr policy, uint flags);
    [DllImport("wintrust.dll", SetLastError=true)]
    static extern bool CryptCATAdminCalcHashFromFileHandle2(IntPtr context, IntPtr file, ref uint length, byte[] hash, uint flags);
    [DllImport("wintrust.dll", SetLastError=true)] static extern bool CryptCATAdminReleaseContext(IntPtr context, uint flags);
    [StructLayout(LayoutKind.Sequential)] struct MemberHead { public uint Size; public IntPtr Tag, FileName; }
    static void CheckCms(byte[] encoded, string expectedThumbprint) {
        var cms = new SignedCms(); cms.Decode(encoded);
        if (cms.SignerInfos.Count != 1 || cms.SignerInfos[0].Certificate == null ||
            !String.Equals(cms.SignerInfos[0].Certificate.Thumbprint, expectedThumbprint, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Package CMS signer is not the exact consent certificate.");
        cms.CheckSignature(true); // Cryptographic verification; no chain trust is granted.
    }
    static byte[] PeCms(string path) {
        byte[] bytes=File.ReadAllBytes(path);
        if (bytes.Length<64 || BitConverter.ToUInt16(bytes,0)!=0x5a4d) throw new InvalidOperationException("Invalid signed PE file.");
        int pe=BitConverter.ToInt32(bytes,0x3c);
        if (pe<0 || pe>bytes.Length-160 || BitConverter.ToUInt32(bytes,pe)!=0x4550) throw new InvalidOperationException("Invalid PE header.");
        int optional=pe+24; ushort magic=BitConverter.ToUInt16(bytes,optional);
        int directories=magic==0x20b ? optional+112 : magic==0x10b ? optional+96 : -1;
        if (directories<0 || directories>bytes.Length-40) throw new InvalidOperationException("Invalid PE directories.");
        uint offset=BitConverter.ToUInt32(bytes,directories+32), size=BitConverter.ToUInt32(bytes,directories+36);
        if (offset>bytes.Length-8 || size<8 || (ulong)offset+size>(ulong)bytes.Length) throw new InvalidOperationException("Missing PE signature.");
        uint length=BitConverter.ToUInt32(bytes,(int)offset);
        if (length<8 || length>size || BitConverter.ToUInt16(bytes,(int)offset+6)!=2 || ((length+7u)&~7u)!=size)
            throw new InvalidOperationException("Unsupported or ambiguous PE signatures.");
        byte[] encoded=new byte[length-8]; Buffer.BlockCopy(bytes,(int)offset+8,encoded,0,encoded.Length); return encoded;
    }
    static void CheckWindowsSignature(string path) {
        var file=new FileInfo { Size=(uint)Marshal.SizeOf(typeof(FileInfo)), Path=path };
        IntPtr pointer=Marshal.AllocHGlobal(Marshal.SizeOf(typeof(FileInfo)));
        try {
            Marshal.StructureToPtr(file,pointer,false);
            var data=new TrustData { Size=(uint)Marshal.SizeOf(typeof(TrustData)), UI=2, Choice=1, File=pointer, StateAction=1, Flags=0x1010 };
            Guid action=new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");
            try {
                int status=WinVerifyTrust(new IntPtr(-1),ref action,ref data);
                if (status!=0 && unchecked((uint)status)!=0x800B0109u)
                    throw new InvalidOperationException("Windows signature integrity/chain validation failed: 0x"+unchecked((uint)status).ToString("X8"));
            } finally { data.StateAction=2; WinVerifyTrust(new IntPtr(-1),ref action,ref data); }
        } finally { Marshal.DestroyStructure(pointer,typeof(FileInfo)); Marshal.FreeHGlobal(pointer); }
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct CatalogInfo {
        public uint Size, Version;
        [MarshalAs(UnmanagedType.LPWStr)] public string Catalog, Tag, Member;
        public IntPtr File, Hash; public uint HashLength; public IntPtr CatalogContext, Admin;
    }
    static bool CheckCatalogMember(string catalog, string path, string tag, string algorithm, bool kernel = false) {
        IntPtr context;
        if (!CryptCATAdminAcquireContext2(out context,IntPtr.Zero,algorithm,IntPtr.Zero,0)) throw new InvalidOperationException("Catalog hash context unavailable.");
        try {
            using(var stream=File.Open(path,FileMode.Open,FileAccess.Read,FileShare.Read)) {
                uint length=0; IntPtr file=stream.SafeFileHandle.DangerousGetHandle();
                if (!CryptCATAdminCalcHashFromFileHandle2(context,file,ref length,null,0) || length==0) throw new InvalidOperationException("Catalog member hash unavailable.");
                byte[] hash=new byte[length];
                if (!CryptCATAdminCalcHashFromFileHandle2(context,file,ref length,hash,0)) throw new InvalidOperationException("Catalog member hash failed.");
                var pinned=GCHandle.Alloc(hash,GCHandleType.Pinned);
                IntPtr pointer=Marshal.AllocHGlobal(Marshal.SizeOf(typeof(CatalogInfo)));
                try {
                    var info=new CatalogInfo { Size=(uint)Marshal.SizeOf(typeof(CatalogInfo)), Catalog=Path.GetFullPath(catalog), Tag=tag,
                        Member=Path.GetFullPath(path), File=file, Hash=pinned.AddrOfPinnedObject(), HashLength=length, Admin=context };
                    Marshal.StructureToPtr(info,pointer,false);
                    var data=new TrustData { Size=(uint)Marshal.SizeOf(typeof(TrustData)), UI=2, Choice=2, File=pointer, StateAction=1, Flags=0x1010 };
                    Guid action=new Guid(kernel ? "F750E6C3-38EE-11D1-85E5-00C04FC295EE" : "00AAC56B-CD44-11d0-8CC2-00C04FC295EE");
                    try {
                        int status=WinVerifyTrust(new IntPtr(-1),ref action,ref data);
                        return status==0 || (!kernel && unchecked((uint)status)==0x800B0109u);
                    } finally { data.StateAction=2; WinVerifyTrust(new IntPtr(-1),ref action,ref data); }
                } finally { Marshal.DestroyStructure(pointer,typeof(CatalogInfo)); Marshal.FreeHGlobal(pointer); pinned.Free(); }
            }
        } finally { CryptCATAdminReleaseContext(context,0); }
    }
    public static void VerifyCatalog(string catalog, string thumbprint, string[] memberFiles) {
        CheckCms(File.ReadAllBytes(catalog),thumbprint); CheckWindowsSignature(catalog);
        VerifyCatalogMembers(catalog,memberFiles,false);
    }
    public static void VerifyKernelCatalog(string catalog, string[] memberFiles) { VerifyCatalogMembers(catalog,memberFiles,true); }
    static void VerifyCatalogMembers(string catalog, string[] memberFiles, bool kernel) {
        IntPtr handle=CryptCATOpen(catalog,0,IntPtr.Zero,0x200,0);
        if(handle==new IntPtr(-1) || handle==IntPtr.Zero) throw new InvalidOperationException("Cannot open driver catalog.");
        try {
            var tags=new System.Collections.Generic.List<string>();
            IntPtr member=IntPtr.Zero;
            while((member=CryptCATEnumerateMember(handle,member))!=IntPtr.Zero) {
                var entry=(MemberHead)Marshal.PtrToStructure(member,typeof(MemberHead));
                tags.Add(Marshal.PtrToStringUni(entry.Tag));
            }
            foreach(string path in memberFiles) {
                bool verified=false;
                foreach(string tag in tags) {
                    if (CheckCatalogMember(catalog,path,tag,"SHA256",kernel) || (!kernel && CheckCatalogMember(catalog,path,tag,"SHA1"))) { verified=true; break; }
                }
                if (!verified) throw new InvalidOperationException("Driver file is not authenticated by this catalog: "+path);
            }
        } finally { CryptCATClose(handle); }
    }
    public static void VerifyTool(string tool, string thumbprint) { CheckCms(PeCms(tool),thumbprint); CheckWindowsSignature(tool); }
    public static void Verify(string catalog, string tool, string thumbprint, string[] memberFiles) {
        VerifyCatalog(catalog,thumbprint,memberFiles);
        VerifyTool(tool,thumbprint);
    }
}
"@
}

function Assert-LocalGamepadCertificatePolicy {
    param([Parameter(Mandatory = $true)]$Certificate)
    if ($certificate.Subject -cne $certificate.Issuer -or $certificate.HasPrivateKey -or
        [DateTime]::Now -lt $certificate.NotBefore -or [DateTime]::Now -gt $certificate.NotAfter) {
        throw 'Local gamepad certificate must be a currently valid self-issued public certificate.'
    }
    $constraints = @($certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.19' })
    $usage = @($certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' })
    if ($constraints.Count -ne 1 -or $constraints[0].CertificateAuthority -or $usage.Count -ne 1 -or
        @($usage[0].EnhancedKeyUsages | Where-Object { $_.Value -eq '1.3.6.1.5.5.7.3.3' }).Count -ne 1 -or
        @($usage[0].EnhancedKeyUsages).Count -ne 1) {
        throw 'Local gamepad trust is restricted to a non-CA certificate used only for code signing.'
    }
}

function Get-PublicLocalGamepadCertificate {
    param([string]$CertificatePath)
    $bytes = [IO.File]::ReadAllBytes($CertificatePath)
    if ([Security.Cryptography.X509Certificates.X509Certificate2]::GetCertContentType($bytes) -ne [Security.Cryptography.X509Certificates.X509ContentType]::Cert) {
        throw 'Local gamepad payload must contain a public DER certificate, never PKCS#12/key material.'
    }
    return [Security.Cryptography.X509Certificates.X509Certificate2]::new($bytes)
}

function Assert-LocalGamepadCryptographicPackage {
    param([Parameter(Mandatory = $true)]$Certificate, [string]$CatalogPath, [string]$ToolPath, [string]$InfPath, [string]$DriverDllPath)
    Assert-LocalGamepadCertificatePolicy -Certificate $Certificate
    Initialize-LocalGamepadTrustValidation
    [VhfLocalTrustValidation]::Verify($CatalogPath, $ToolPath, $Certificate.Thumbprint.ToUpperInvariant(), @($InfPath, $DriverDllPath))
}
function Add-ValidatedLocalGamepadCertificate {
    param([Parameter(Mandatory = $true)]$Certificate, [ValidateSet('Root', 'TrustedPublisher')][string]$StoreName)
    $store = [Security.Cryptography.X509Certificates.X509Store]::new($StoreName, 'LocalMachine')
    try {
        $store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        if ($store.Certificates.Find([Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $Certificate.Thumbprint, $false).Count -gt 0) { return $false }
        $store.Add($Certificate)
        return $true
    } finally { $store.Close() }
}

function Remove-AddedLocalGamepadTrust {
    param($State)
    if ($null -eq $State) { return }
    foreach ($name in @($State.AddedStores)) {
        try { Remove-ValidatedLocalGamepadCertificate -Certificate $State.Certificate -StoreName $name } catch {
            Write-Warning "Unable to roll back newly added gamepad certificate $($State.Certificate.Thumbprint) in LocalMachine\${name}: $($_.Exception.Message)"
        }
    }
}

function Remove-ValidatedLocalGamepadCertificate {
    param([Parameter(Mandatory = $true)]$Certificate, [ValidateSet('Root', 'TrustedPublisher')][string]$StoreName)
    $store = [Security.Cryptography.X509Certificates.X509Store]::new($StoreName, 'LocalMachine')
    try {
        $store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $matches = $store.Certificates.Find([Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $Certificate.Thumbprint, $false)
        foreach ($exactCertificate in $matches) { $store.Remove($exactCertificate) }
    } finally { $store.Close() }
}

function Ensure-ConsentedLocalGamepadTrust {
    param([Parameter(Mandatory = $true)]$Certificate, [int]$Consent = 0, [string]$ConsentThumbprint = '')
    $thumbprint = Get-Thumbprint $Certificate
    $state = [pscustomobject]@{ Certificate = $Certificate; AddedStores = @() }
    $missingStores = @()
    foreach ($name in @('Root', 'TrustedPublisher')) {
        $present = @(Get-ChildItem -LiteralPath "Cert:\LocalMachine\$name" -ErrorAction Stop | Where-Object { (Get-Thumbprint $_) -ceq $thumbprint })
        if ($present.Count -eq 0) { $missingStores += $name }
    }
    if ($missingStores.Count -eq 0) { return $state }
    if ($Consent -ne 1 -or $ConsentThumbprint -notmatch '^[0-9a-fA-F]{40}$' -or $ConsentThumbprint.ToUpperInvariant() -cne $thumbprint) {
        throw "VIRTUAL_GAMEPAD_DRIVER_WARNING: Explicit recipient consent is required to trust bundled certificate '$($Certificate.Subject)' ($thumbprint) in LocalMachine Root and TrustedPublisher. Trust persists and can authorize other code signed by this key. A new daily certificate requires new consent. Driver retained; no trust changes made."
    }
    Write-DriverMessage "Recipient approved persistent machine-wide trust of '$($Certificate.Subject)' ($thumbprint) for this validated VHF package."
    try {
        foreach ($name in $missingStores) {
            if (Add-ValidatedLocalGamepadCertificate -Certificate $Certificate -StoreName $name) { $state.AddedStores += $name }
        }
        return $state
    } catch {
        Remove-AddedLocalGamepadTrust -State $state
        throw
    }
}

function Assert-DriverPackage {
    param([Parameter(Mandatory = $true)][string] $Root, [switch]$AllowUntrustedLocalPackage, [switch]$DeferMapper)

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
        (@($manifest.signing.signed_downstream) -join "`n") -cne ($downstreamSignedFiles -join "`n")) {
        throw '[VibeshineVhfGamepad] MSI-signing manifest does not declare the exact downstream-signed payloads.'
    }
    Assert-ManifestPayloadContract -Manifest $manifest -Root $Root -DeferMapper:$DeferMapper

    $certificatePath = Join-Path $Root ($localTestCertificate -replace '/', '\\')
    $hasLocalCertificate = Test-Path -LiteralPath $certificatePath -PathType Leaf
    if ($hasLocalCertificate) {
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
        $localCertificate = Assert-LocalTestCertificateIdentity -CertificatePath $certificatePath -CatalogSignature $catalogSignature -ToolSignature $toolSignature
        $catalogSignature = Get-AuthenticodeSignature -LiteralPath $catalogPath
        $toolSignature = Get-AuthenticodeSignature -LiteralPath $toolPath
    }

    $validatedLocalCertificate = $null
    if ($hasLocalCertificate -and $AllowUntrustedLocalPackage) {
        $manifestParts = @($manifest.driver_ver -split ',')
        if ($manifestParts.Count -ne 2 -or (Get-InfComponentVersion (Join-Path $Root 'driver/VibeshineVhfGamepad.inf')) -ne (ConvertTo-DriverComponentVersion $manifestParts[1].Trim())) {
            throw 'Local gamepad manifest does not match the actual INF component version.'
        }
        Assert-LocalGamepadCryptographicPackage -Certificate $localCertificate -CatalogPath $catalogPath -ToolPath $toolPath -InfPath (Join-Path $Root 'driver/VibeshineVhfGamepad.inf') -DriverDllPath (Join-Path $Root 'driver/VibeshineVhfGamepad.dll')
        $validatedLocalCertificate = $localCertificate
    }
    # PowerShell can map an untrusted self-signed root to UnknownError. This
    # is eligible only after the native verifier proved the exact CMS signer,
    # Windows signature integrity (success or CERT_E_UNTRUSTEDROOT only), and
    # catalog membership. It is never permission to stage an untrusted driver.
    $allowedStatuses = if ($null -ne $validatedLocalCertificate) { @('Valid', 'NotTrusted', 'UnknownError') } else { @('Valid') }
    if ([string]$catalogSignature.Status -notin $allowedStatuses -or [string]$toolSignature.Status -notin $allowedStatuses) {
        throw '[VibeshineVhfGamepad] Catalog or root-device setup tool is not trusted by this Windows installation.'
    }

    $mapperInfPath = $null
    if (-not $DeferMapper -and (Test-MouseMapperFeature -Manifest $manifest)) {
        $mapperInfPath = Join-Path $Root 'mouse_mapper/VibeshineVhfMouseMapper.inf'
        $mapperCatalogPath = Join-Path $Root 'mouse_mapper/VibeshineVhfMouseMapper.cat'
        $mapperSysPath = Join-Path $Root 'mouse_mapper/VibeshineVhfMouseMapper.sys'
        Initialize-LocalGamepadTrustValidation
        if ($hasLocalCertificate -and $manifest.mouse_mapper.kernel_signing -ceq 'self-signed-local-test') {
            [VhfLocalTrustValidation]::VerifyCatalog($mapperCatalogPath, $localCertificate.Thumbprint, @($mapperInfPath, $mapperSysPath))
            [VhfLocalTrustValidation]::VerifyTool($mapperSysPath, $localCertificate.Thumbprint)
            if (-not [VhfLocalTrustValidation]::IsTestSigningEnabled()) { throw 'The nightly mouse mapper is signed, but Windows test-signing is not enabled. Certificate trust alone does not authorize kernel-driver loading. Boot security settings were left unchanged.' }
        } else {
            if ($manifest.mouse_mapper.kernel_signing -cne 'windows-kernel-policy') { throw 'The bundled mapper is an unsigned producer artifact and is not eligible for kernel installation. A separately trusted kernel-signed mapper package is required; publishing or MSI signing does not grant that trust.' }
            [VhfLocalTrustValidation]::VerifyKernelCatalog($mapperCatalogPath, @($mapperInfPath, $mapperSysPath))
        }
    }

    return [PSCustomObject]@{
        Root = $Root
        Manifest = $manifest
        MapperInfPath = $mapperInfPath
        InfPath = Join-Path $Root 'driver/VibeshineVhfGamepad.inf'
        SetupToolPath = $toolPath
        ProductionPublisherCertificate = $productionPublisherCertificate
        LocalCertificate = $validatedLocalCertificate
        CatalogPath = $catalogPath
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
    if ('VibeshineDriverInventory' -as [type]) { [VibeshineDriverInventory]::Invalidate() }
    try {
        & $pnputil @Arguments | Out-Host
        $exitCode = $LASTEXITCODE
    } finally {
        if ('VibeshineDriverInventory' -as [type]) { [VibeshineDriverInventory]::Invalidate() }
    }
    if ($exitCode -notin $AllowedExitCodes) {
        throw "[VibeshineVhfGamepad] pnputil failed with exit code $exitCode."
    }
    return $exitCode
}

function Remove-DriverStorePackage {
    if (Get-Command Get-SharedDriverInventory -ErrorAction SilentlyContinue) {
        $packages = @((Get-SharedDriverInventory -InfName 'VibeshineVhfGamepad.inf' -HardwareId 'ROOT\VIBESHINEVIRTUALGAMEPAD').Packages)
        $packages += @((Get-SharedDriverInventory -InfName 'VibeshineVhfMouseMapper.inf' -HardwareId 'ROOT\VIBESHINEVIRTUALGAMEPAD').Packages)
    } else {
    $command = Get-Command Get-WindowsDriver -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw '[VibeshineVhfGamepad] Get-WindowsDriver is unavailable; refusing to guess Driver Store package names.'
    }

    $packages = @(Get-WindowsDriver -Online -ErrorAction Stop | Where-Object {
        [IO.Path]::GetFileName([string] $_.OriginalFileName) -in @('VibeshineVhfGamepad.inf', 'VibeshineVhfMouseMapper.inf')
    })
    }
    $rebootRequired = $false
    foreach ($package in $packages) {
        $published = [string]$package.Driver
        if ($published -notmatch '^oem\d+\.inf$') { throw 'Unknown owned package identity; Driver Store removal deferred.' }
        Write-DriverMessage "Removing owned Driver Store package $published."
        $exitCode = Invoke-PnpUtil -Arguments @('/delete-driver', $published, '/uninstall')
        $rebootRequired = $rebootRequired -or ($exitCode -eq $rebootExitCode)
    }
    return $rebootRequired
}

function Get-MapperInstallDeclaration {
    param([Parameter(Mandatory = $true)][string]$Root)
    $manifestPath = Join-Path $Root 'manifest.json'
    Assert-File -Path $manifestPath
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if (Test-MouseMapperDeclared -Manifest $manifest) { return $manifest }
    return $null
}

function Get-MapperHardwareId {
    param([Parameter(Mandatory = $true)][string]$InfPath)
    $matches = [regex]::Matches((Get-Content -LiteralPath $InfPath -Raw), 'HID\\VID_([0-9A-Fa-f]{4})&PID_([0-9A-Fa-f]{4})&REV_0100&Col02')
    $ids = @($matches | ForEach-Object { $_.Value.ToUpperInvariant() } | Sort-Object -Unique)
    if ($ids.Count -ne 1) { throw 'Mapper INF lacks one exact VID/PID revision/collection binding.' }
    return $ids[0]
}

function Assert-MapperInstallCompatibility {
    param([Parameter(Mandatory = $true)][string]$InfPath, [Parameter(Mandatory = $true)]$Manifest)
    $hardwareId = Get-MapperHardwareId -InfPath $InfPath
    $expectedId = 'HID\VID_{0:X4}&PID_{1:X4}&REV_0100&COL02' -f [uint16]$Manifest.mouse_mapper.vendor_id, [uint16]$Manifest.mouse_mapper.product_id
    if ($hardwareId -cne $expectedId -or $Manifest.mouse_mapper.transport_version -ne 1) { throw 'Mapper identity/transport metadata does not match its catalog-bound INF.' }
    $target = Get-InfComponentVersion -Path $InfPath
    if (Get-Command Get-SharedDriverInventory -ErrorAction SilentlyContinue) {
        $inventory = Get-SharedDriverInventory -InfName 'VibeshineVhfMouseMapper.inf' -HardwareId $hardwareId
        $packages = @($inventory.Packages); $devices = @($inventory.Devices)
    } else {
    $packages = @(Get-WindowsDriver -Online -ErrorAction Stop | Where-Object {
        [IO.Path]::GetFileName([string]$_.OriginalFileName) -ieq 'VibeshineVhfMouseMapper.inf'
    })
    $devices = @(Get-CimInstance -ClassName Win32_PnPSignedDriver -ErrorAction Stop | Where-Object {
        ([string]$_.DeviceID).StartsWith($hardwareId, [StringComparison]::OrdinalIgnoreCase) -or
        ([string]$_.HardwareID).Equals($hardwareId, [StringComparison]::OrdinalIgnoreCase)
    })
    }
    foreach ($package in $packages) {
        $path = [string]$package.OriginalFileName
        if (-not [IO.Path]::IsPathRooted($path)) {
            if ([string]$package.Driver -notmatch '^oem\d+\.inf$') { throw 'Unknown mapper Driver Store identity; both drivers retained.' }
            $path = Join-Path (Join-Path $env:windir 'INF') ([string]$package.Driver)
        }
        if ((Get-InfComponentVersion -Path $path) -gt $target) { throw 'A newer mapper package exists; refusing to stage an older mapper. Supply a compatible package at least as new. Both drivers retained.' }
    }
    foreach ($device in $devices) {
        if ((ConvertTo-DriverComponentVersion ([string]$device.DriverVersion)) -gt $target) { throw 'A newer active mapper exists; refusing to downgrade it. Both drivers retained.' }
    }
    # Equal Driver Store entries are deliberately not a skip: /install repairs
    # a missing or failed binding on retry without replacing the parent.
}

function Test-MapperParentInstallRequired {
    param([Parameter(Mandatory = $true)][string]$InfPath)
    $target = Get-InfComponentVersion -Path $InfPath
    if (Get-Command Get-SharedDriverInventory -ErrorAction SilentlyContinue) {
        $inventory = Get-SharedDriverInventory -InfName 'VibeshineVhfGamepad.inf' -HardwareId 'ROOT\VIBESHINEVIRTUALGAMEPAD'
        $packages = @($inventory.Packages); $devices = @($inventory.Devices)
    } else {
    $packages = @(Get-WindowsDriver -Online -ErrorAction Stop | Where-Object {
        [IO.Path]::GetFileName([string]$_.OriginalFileName) -ieq 'VibeshineVhfGamepad.inf'
    })
    $devices = @(Get-CimInstance -ClassName Win32_PnPSignedDriver -ErrorAction Stop | Where-Object {
        ([string]$_.DeviceID).StartsWith('ROOT\VIBESHINEVIRTUALGAMEPAD\', [StringComparison]::OrdinalIgnoreCase) -or
        ([string]$_.HardwareID).Equals('ROOT\VIBESHINEVIRTUALGAMEPAD', [StringComparison]::OrdinalIgnoreCase)
    })
    }
    foreach ($package in $packages) {
        $path = [string]$package.OriginalFileName
        if (-not [IO.Path]::IsPathRooted($path)) {
            if ([string]$package.Driver -notmatch '^oem\d+\.inf$') { throw 'Unknown parent Driver Store identity; both drivers retained.' }
            $path = Join-Path (Join-Path $env:windir 'INF') ([string]$package.Driver)
        }
        if ((Get-InfComponentVersion -Path $path) -gt $target) {
            throw 'A newer parent package is staged; this candidate cannot attest its mouse identity/transport. Supply a matching package at least as new. Both drivers retained; no trust or driver changes made.'
        }
    }
    $requiresInstall = $devices.Count -eq 0
    foreach ($device in $devices) {
        $active = ConvertTo-DriverComponentVersion ([string]$device.DriverVersion)
        if ($active -gt $target) {
            throw 'A newer parent is active; this candidate cannot attest its mouse identity/transport. Supply a matching package at least as new. Both drivers retained; no trust or driver changes made.'
        }
        if ($active -lt $target) { $requiresInstall = $true }
    }
    # A stored package is not an active root. Equal staged-only versions must
    # reach setup to create/verify the root after staging the mapper. Only an
    # equal active parent can retain its binding without invoking setup.
    return $requiresInstall
}

function Assert-OptionalMapperPackage {
    param([Parameter(Mandatory=$true)]$Package)
    $manifest = $Package.Manifest
    $root = $Package.Root
    if (-not (Test-MouseMapperFeature -Manifest $manifest)) { return $null }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    try {
        foreach ($relative in @(Get-MouseMapperPayload)) { Assert-ManifestHash -Manifest $manifest -Root $root -RelativePath $relative }
        $inf = Join-Path $root 'mouse_mapper/VibeshineVhfMouseMapper.inf'
        $cat = Join-Path $root 'mouse_mapper/VibeshineVhfMouseMapper.cat'
        $sys = Join-Path $root 'mouse_mapper/VibeshineVhfMouseMapper.sys'
        Initialize-LocalGamepadTrustValidation
        if ($null -ne $Package.LocalCertificate -and $manifest.mouse_mapper.kernel_signing -ceq 'self-signed-local-test') {
            [VhfLocalTrustValidation]::VerifyCatalog($cat, $Package.LocalCertificate.Thumbprint, @($inf, $sys))
            [VhfLocalTrustValidation]::VerifyTool($sys, $Package.LocalCertificate.Thumbprint)
            if (-not [VhfLocalTrustValidation]::IsTestSigningEnabled()) {
                throw 'The nightly mouse mapper is signed, but active Windows policy does not enable test-signing. Certificate trust alone does not authorize kernel loading. Security settings were left unchanged.'
            }
        } elseif ($manifest.mouse_mapper.kernel_signing -ceq 'windows-kernel-policy') {
            [VhfLocalTrustValidation]::VerifyKernelCatalog($cat, @($inf, $sys))
        } else {
            throw 'The bundled mouse mapper has no eligible signature; the base gamepad package is validated separately.'
        }
        Assert-MapperInstallCompatibility -InfPath $inf -Manifest $manifest
        return $inf
    } finally {
        Write-DriverMessage "DRIVER_OPERATION:mapper-validation elapsed_ms=$($clock.ElapsedMilliseconds)"
    }
}

function Install-DriverPackage {
    Assert-Administrator
    $localTest = Test-Path -LiteralPath (Join-Path $PSScriptRoot 'driver/VibeshineVhfGamepad.cer')
    $mapperDeclaration = Get-MapperInstallDeclaration -Root $PSScriptRoot
    $installParent = Test-SafeDriverInstall -InfPath (Join-Path $PSScriptRoot 'driver/VibeshineVhfGamepad.inf') -CatalogPath (Join-Path $PSScriptRoot 'driver/VibeshineVhfGamepad.cat') -HardwareId 'ROOT\VIBESHINEVIRTUALGAMEPAD' -WarningMarker 'VIRTUAL_GAMEPAD_DRIVER_WARNING' -LocalTestPackage $localTest -AllowUntrustedLocalPackage $localTest
    if ($null -ne $mapperDeclaration) {
        $installParent = Test-MapperParentInstallRequired -InfPath (Join-Path $PSScriptRoot 'driver/VibeshineVhfGamepad.inf')
    }
    if (-not $installParent -and $null -eq $mapperDeclaration) { return 0 }
    $validationClock = [Diagnostics.Stopwatch]::StartNew()
    $package = Assert-DriverPackage -Root $PSScriptRoot -AllowUntrustedLocalPackage -DeferMapper
    Write-DriverMessage "DRIVER_OPERATION:base-package-validation elapsed_ms=$($validationClock.ElapsedMilliseconds)"
    if ($null -ne $mapperDeclaration) {
        # The optional ordered mouse child has no dependency from the base
        # controller path. Verify it independently, before any attempt to stage.
        try {
            $package.MapperInfPath = Assert-OptionalMapperPackage -Package $package
        } catch {
            Write-DriverMessage "VIRTUAL_MOUSE_MAPPER_WARNING: $($_.Exception.Message) Base gamepad installation continues independently."
        }
    }
    $trustState = $null
    try {
        if ($null -ne $package.LocalCertificate -and ($installParent -or $null -ne $package.MapperInfPath)) {
            $trustConsent = $AllowLocalGamepadCertificateTrust
            $trustConsentThumbprint = $ConsentCertificateThumbprint
            if ($package.LocalCertificate.Thumbprint -ine '7F1868BF86698A8674F40E686B02C8C702BD3B88') {
                # Other local signers may be used only when already trusted in
                # both stores; never import a new identity through beta consent.
                $trustConsent = 0
                $trustConsentThumbprint = ''
            }
            $trustState = Ensure-ConsentedLocalGamepadTrust -Certificate $package.LocalCertificate -Consent $trustConsent -ConsentThumbprint $trustConsentThumbprint
            if ((Get-AuthenticodeSignature -LiteralPath $package.CatalogPath).Status -ne 'Valid' -or (Get-AuthenticodeSignature -LiteralPath $package.SetupToolPath).Status -ne 'Valid') {
                throw 'Windows still does not trust the validated VHF package after approved certificate setup. Driver retained; boot/signing settings unchanged.'
            }
        }
        if ($installParent -and $DailyUnsignedPolicy -eq 0 -and $null -ne $package.ProductionPublisherCertificate) {
            Ensure-ProductionPublisherTrusted `
                -PublisherCertificate $package.ProductionPublisherCertificate `
                -ExpectedSubject $expectedSignPathFoundationSignerSubject
        }
        # PnPUtil performs Windows' catalog-to-INF/DLL membership validation while
        # it stages the package. The installed product deliberately does not ship
        # SDK-only signtool.exe just to repeat that platform validation.
        $mapperExitCode = 0
        $mapperStaged = $false
        if ($null -ne $package.MapperInfPath) {
            # Stage and bind the mapper before starting/restarting the UMDF parent.
            try {
                $mapperExitCode = Invoke-PnpUtil -Arguments @('/add-driver', $package.MapperInfPath, '/install')
                $mapperStaged = $true
            } catch {
                Write-DriverMessage "VIRTUAL_MOUSE_MAPPER_WARNING: Mapper staging failed: $($_.Exception.Message) Base gamepad installation continues independently."
            }
        }
        if (-not $installParent) {
            if ($mapperStaged) {
                Write-DriverMessage 'Retained the equal-version active parent; staged the validated mapper payload. Runtime readiness is checked when input opens.'
            } else {
                Write-DriverMessage 'Retained the equal-version active parent; optional mapper was not installed.'
            }
            if ($mapperExitCode -eq $rebootExitCode) { Write-DriverMessage 'VIRTUAL_GAMEPAD_RESTART_REQUIRED'; return $rebootExitCode }
            return 0
        }
        $pnputilExitCode = Invoke-PnpUtil -Arguments @('/add-driver', $package.InfPath)
        if ('VibeshineDriverInventory' -as [type]) { [VibeshineDriverInventory]::Invalidate() }
        try {
            & $package.SetupToolPath install --inf $package.InfPath | Out-Host
            $setupExitCode = $LASTEXITCODE
        } finally {
            if ('VibeshineDriverInventory' -as [type]) { [VibeshineDriverInventory]::Invalidate() }
        }
        if ($setupExitCode -ne 0 -and $setupExitCode -ne $rebootExitCode) {
            throw "[VibeshineVhfGamepad] Root-device setup failed with exit code $setupExitCode."
        }
        # The setup tool's zero exit code is a stronger result than PnPUtil's
        # advisory 3010: it has reloaded the exact owned source node, opened its
        # private interface, completed the versioned query handshake, and (when
        # PnP requested a restart) attested the selected DriverVer. Do not request
        # a reboot that Windows no longer needs just because the initial Driver
        # Store staging step asked for one.
        if ($setupExitCode -eq 0 -and $mapperExitCode -ne $rebootExitCode) {
            if ($pnputilExitCode -eq $rebootExitCode) {
                Write-DriverMessage 'Windows requested restart while staging the package, but the VHF source device is live now.'
            }
            Write-DriverMessage 'Installed and verified the VHF source device.'
            return 0
        }
        if ($mapperExitCode -eq $rebootExitCode -or $pnputilExitCode -eq $rebootExitCode -or $setupExitCode -eq $rebootExitCode) {
            Write-DriverMessage 'VIRTUAL_GAMEPAD_RESTART_REQUIRED'
            return $rebootExitCode
        }
        throw "[VibeshineVhfGamepad] Root-device setup ended with unexpected exit code $setupExitCode."
    } catch {
        Remove-AddedLocalGamepadTrust -State $trustState
        throw
    }
}

function Uninstall-DriverPackage {
    Assert-Administrator
    $package = Assert-DriverPackage -Root $PSScriptRoot -DeferMapper
    if ('VibeshineDriverInventory' -as [type]) { [VibeshineDriverInventory]::Invalidate() }
    try {
        & $package.SetupToolPath remove | Out-Host
        $setupExitCode = $LASTEXITCODE
    } finally {
        if ('VibeshineDriverInventory' -as [type]) { [VibeshineDriverInventory]::Invalidate() }
    }
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
    Write-DriverMessage 'Removed only Vibepollo-managed VHF source devices.'
    return 0
}

if ($FunctionsOnly) { return }
if (Test-Path -LiteralPath (Join-Path $PSScriptRoot '../driver-inventory.ps1')) {
    . (Join-Path $PSScriptRoot '../driver-inventory.ps1')
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
