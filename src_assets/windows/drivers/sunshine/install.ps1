param(
    [switch]$Uninstall,
    [switch]$ValidateOnly,
    [switch]$AllowUnsignedCatalogForValidation,
    [switch]$RegisterVulkanLayerOnly,
    [switch]$UnregisterVulkanLayerOnly,
    [ValidateSet(0, 1)]
    [int]$DailyUnsignedPolicy = 0,
    [switch]$InstallerBestEffort
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $PSCommandPath
$hardwareId = 'Root\SunshineVirtualDisplay'
$classGuid = '{4D36E968-E325-11CE-BFC1-08002BE10318}'
$deviceGroupId = 'SunshineVirtualDisplayGroup'
$nefConc = Join-Path $scriptDir 'nefconc.exe'
$infPath = Join-Path $scriptDir 'SunshineVirtualDisplayDriver.inf'
$dllPath = Join-Path $scriptDir 'SunshineVirtualDisplayDriver.dll'
$catPath = Join-Path $scriptDir 'SunshineVirtualDisplayDriver.cat'
$certPath = Join-Path $scriptDir 'SunshineVirtualDisplayDriver.cer'
$signPathSignerSubject = 'CN=SignPath Foundation, O=SignPath Foundation, L=Lewes, S=Delaware, C=US'
$probePath = Join-Path $scriptDir 'virtualdisplay_probe.exe'
$vulkanLayerDir = Join-Path $scriptDir 'vulkan-layer'
$vulkanLayerDllPath = Join-Path $vulkanLayerDir 'VkLayer_sunshine_hdr.dll'
$vulkanLayerJsonPath = Join-Path $vulkanLayerDir 'VkLayer_sunshine_hdr.json'
$vulkanLayers = @(
    @{ View = [Microsoft.Win32.RegistryView]::Registry64; Manifest = $vulkanLayerJsonPath; Dll = $vulkanLayerDllPath; Machine = 0x8664 },
    @{ View = [Microsoft.Win32.RegistryView]::Registry32; Manifest = (Join-Path $vulkanLayerDir 'VkLayer_sunshine_hdr_x86.json'); Dll = (Join-Path $vulkanLayerDir 'VkLayer_sunshine_hdr_x86.dll'); Machine = 0x014c }
)
$vulkanImplicitLayersSubKey = 'SOFTWARE\Khronos\Vulkan\ImplicitLayers'
$userModeDriversSid = 'S-1-5-84-0-0-0-0-0'
# Sticky for this invocation: an old active driver passing a probe cannot
# make changes which Windows deferred until reboot effective.
$script:rebootRequired = $false
$script:apolloServiceWasRunning = $false
$script:virtualDisplayBrokerWasRunning = $false

trap {
    $failure = $_
    if ($script:rebootRequired) {
        Write-DriverRestartRequired
    }
    try {
        Start-VirtualDisplayBrokerIfNeeded
    } catch {
        Write-Warning "[SunshineVirtualDisplay] Unable to restore the virtual display broker after failure: $($_.Exception.Message)"
    }
    try {
        Start-ApolloServiceIfNeeded
    } catch {
        Write-Warning "[SunshineVirtualDisplay] Unable to restore the Apollo service after failure: $($_.Exception.Message)"
    }

    if ($InstallerBestEffort) {
        Write-Warning '[SunshineVirtualDisplay] VIRTUAL_DISPLAY_DRIVER_WARNING: Optional virtual display driver setup did not complete.'
        Write-Warning "[SunshineVirtualDisplay] Installer best-effort driver action failed: $($failure.Exception.Message)"
        exit 0
    }

    throw
}

function Write-DriverRestartRequired {
    Write-Warning '[SunshineVirtualDisplay] VIRTUAL_DISPLAY_RESTART_REQUIRED: Windows restart is required to finalize virtual display driver changes; further driver repair is deferred.'
    Write-Host '[SunshineVirtualDisplay] A reboot is required to finalize driver installation.'
}

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
        [bool]$LocalTestPackage = $false
    )
    $target = Get-InfComponentVersion $InfPath
    $infName = [IO.Path]::GetFileName($InfPath)
    # Both queries must succeed. An empty successful query means missing;
    # errors, missing active versions, and unreadable store INFs mean unknown.
    $packages = @(Get-WindowsDriver -Online -All -ErrorAction Stop | Where-Object {
        [IO.Path]::GetFileName([string]$_.OriginalFileName) -ieq $infName
    })
    $devices = @(Get-CimInstance -ClassName Win32_PnPSignedDriver -ErrorAction Stop | Where-Object {
        ([string]$_.DeviceID).StartsWith($HardwareId, [StringComparison]::OrdinalIgnoreCase) -or
        ([string]$_.HardwareID).Equals($HardwareId, [StringComparison]::OrdinalIgnoreCase)
    })
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
    if ($LocalTestPackage) { throw "$WarningMarker`: Unreviewed local display package rejected; installed driver retained." }
    # Only the pinned release signer or the existing trusted production publisher
    # may become eligible; verify INF/DLL catalog membership before any trust.
    $null = Assert-ExpectedDisplayPackage -InfPath $InfPath -CatalogPath $CatalogPath -HardwareId $HardwareId
    return $true
}
# DRIVER_INSTALL_SAFETY_END

function Initialize-DisplayPackageValidation {
    if ('DisplayPackageValidation' -as [type]) { return }
    Add-Type -ReferencedAssemblies @('System', 'System.Core', 'System.Security') -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography.Pkcs;
public static class DisplayPackageValidation {
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
            throw new InvalidOperationException("Package CMS signer is not the expected release certificate.");
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
    static bool CheckCatalogMember(string catalog, string path, string tag, string algorithm) {
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
                    Guid action=new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");
                    try {
                        int status=WinVerifyTrust(new IntPtr(-1),ref action,ref data);
                        return status==0 || unchecked((uint)status)==0x800B0109u;
                    } finally { data.StateAction=2; WinVerifyTrust(new IntPtr(-1),ref action,ref data); }
                } finally { Marshal.DestroyStructure(pointer,typeof(CatalogInfo)); Marshal.FreeHGlobal(pointer); pinned.Free(); }
            }
        } finally { CryptCATAdminReleaseContext(context,0); }
    }
    public static void VerifyCatalog(string catalog, string thumbprint, string[] memberFiles) {
        CheckCms(File.ReadAllBytes(catalog),thumbprint); CheckWindowsSignature(catalog);
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
                    if (CheckCatalogMember(catalog,path,tag,"SHA256") || CheckCatalogMember(catalog,path,tag,"SHA1")) { verified=true; break; }
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

function Assert-ExpectedDisplayPackage {
    param([string]$InfPath = $script:infPath, [string]$CatalogPath = $script:catPath,
          [string]$HardwareId = $script:hardwareId, [string]$CertificatePath = $script:certPath,
          [string]$DriverPath = $script:dllPath)
    $isSudo = $HardwareId -ieq 'ROOT\SUDOMAKER\SUDOVDA'
    if (!$isSudo -and $HardwareId -ine 'ROOT\SunshineVirtualDisplay') { throw 'Unknown display hardware identity.' }
    $expectedPin = if ($isSudo) { '3C918FC73525AD8B1521B6DB26B71F694277CC49' } else { '37E100B2B382866FA7CE04EE33FE53DF53AEDC6D' }
    $expectedInf = if ($isSudo) { 'SudoVDA.inf' } else { 'SunshineVirtualDisplayDriver.inf' }
    $expectedDll = if ($isSudo) { 'SudoVDA.dll' } else { 'SunshineVirtualDisplayDriver.dll' }
    $expectedCat = if ($isSudo) { 'sudovda.cat' } else { 'SunshineVirtualDisplayDriver.cat' }
    if ([IO.Path]::GetFileName($InfPath) -ine $expectedInf -or
        [IO.Path]::GetFileName($DriverPath) -ine $expectedDll -or
        [IO.Path]::GetFileName($CatalogPath) -ine $expectedCat) { throw 'Display package filenames do not match the hardware identity.' }
    $inf = (Get-Content -LiteralPath $InfPath -ErrorAction Stop | ForEach-Object { ($_ -split ';',2)[0] }) -join "`n"
    if ($inf -notmatch '(?im)^\s*Class\s*=\s*Display\s*$' -or
        $inf -notmatch '(?im)^\s*ClassGuid\s*=\s*\{4d36e968-e325-11ce-bfc1-08002be10318\}\s*$' -or
        $inf -notmatch ('(?im)^\s*CatalogFile\s*=\s*' + [regex]::Escape($expectedCat) + '\s*$') -or
        $inf -notmatch ('(?im)^\s*%[^%]+%\s*=\s*[^,\r\n]+,\s*' + [regex]::Escape($HardwareId) + '\s*$')) {
        throw 'Display INF class, catalog or hardware identity mismatch.'
    }
    Initialize-DisplayPackageValidation
    $cms = New-Object Security.Cryptography.Pkcs.SignedCms
    $cms.Decode([IO.File]::ReadAllBytes($CatalogPath))
    if ($cms.SignerInfos.Count -ne 1 -or !$cms.SignerInfos[0].Certificate) { throw 'Display catalog must have one identifiable signer.' }
    $signer = $cms.SignerInfos[0].Certificate
    $selfSigned = [string]::Equals($signer.Subject,$signer.Issuer,[StringComparison]::OrdinalIgnoreCase)
    if ($selfSigned) {
        $bytes = [IO.File]::ReadAllBytes($CertificatePath)
        if ([Security.Cryptography.X509Certificates.X509Certificate2]::GetCertContentType($bytes) -ne [Security.Cryptography.X509Certificates.X509ContentType]::Cert) { throw 'Display package certificate must be a public DER certificate.' }
        $bundled = [Security.Cryptography.X509Certificates.X509Certificate2]::new($bytes)
        if ($bundled.HasPrivateKey -or $bundled.Thumbprint -ine $expectedPin -or $signer.Thumbprint -ine $expectedPin -or
            [DateTime]::Now -lt $bundled.NotBefore -or [DateTime]::Now -gt $bundled.NotAfter) { throw 'Display catalog does not match the pinned, currently valid release certificate.' }
        $eku = @($bundled.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' })
        $constraints = @($bundled.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.19' })
        if ($eku.Count -ne 1 -or @($eku[0].EnhancedKeyUsages).Count -ne 1 -or $eku[0].EnhancedKeyUsages[0].Value -ne '1.3.6.1.5.5.7.3.3' -or
            @($constraints | Where-Object { $_.CertificateAuthority }).Count -ne 0) { throw 'Pinned display certificate is not a non-CA code-signing certificate.' }
    } else {
        # Retain the existing production publisher identity and Windows chain validation.
        $signature = Get-AuthenticodeSignature -LiteralPath $CatalogPath -ErrorAction Stop
        if ($isSudo -or $signer.Subject -ine 'CN=SignPath Foundation, O=SignPath Foundation, L=Lewes, S=Delaware, C=US' -or
            $signature.Status -ne 'Valid' -or !$signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ine $signer.Thumbprint) {
            throw 'Display production catalog publisher or trusted chain mismatch.'
        }
    }
    [DisplayPackageValidation]::VerifyCatalog($CatalogPath, $signer.Thumbprint, @($InfPath,$DriverPath))
    return [pscustomobject]@{ Certificate=$signer; SelfSigned=$selfSigned; CatalogPath=$CatalogPath }
}

function Test-DisplayCertificatePresent {
    param($Certificate,[string]$StoreName)
    $store = [Security.Cryptography.X509Certificates.X509Store]::new($StoreName,'LocalMachine')
    try {
        $store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly)
        return $store.Certificates.Find([Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,$Certificate.Thumbprint,$false).Count -gt 0
    } finally { $store.Close() }
}
function Add-DisplayCertificate {
    param($Certificate,[string]$StoreName)
    $store = [Security.Cryptography.X509Certificates.X509Store]::new($StoreName,'LocalMachine')
    try { $store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite); $store.Add($Certificate) } finally { $store.Close() }
}
function Remove-DisplayCertificate {
    param($Certificate,[string]$StoreName)
    $store = [Security.Cryptography.X509Certificates.X509Store]::new($StoreName,'LocalMachine')
    try {
        $store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        foreach ($entry in $store.Certificates.Find([Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,$Certificate.Thumbprint,$false)) { $store.Remove($entry) }
    } finally { $store.Close() }
}
function Undo-DisplayTrustProvisioning {
    foreach ($entry in @($script:displayTrustAdded)) {
        try { Remove-DisplayCertificate -Certificate $entry.Certificate -StoreName $entry.StoreName }
        catch { Write-Warning "VIRTUAL_DISPLAY_DRIVER_WARNING: Failed to roll back newly added display publisher trust: $($_.Exception.Message)" }
    }
    $script:displayTrustAdded = @()
}
function Install-ExpectedDisplayTrust {
    # Repeat independent verification immediately before trust mutation.
    $package = Assert-ExpectedDisplayPackage
    $script:displayTrustAdded = @()
    try {
        $stores = if ($package.SelfSigned) { @('Root','TrustedPublisher') } else { @('TrustedPublisher') }
        foreach ($storeName in $stores) {
            if (!(Test-DisplayCertificatePresent -Certificate $package.Certificate -StoreName $storeName)) {
                # Record ownership before Add: rollback handles a partial Add failure.
                $script:displayTrustAdded += [pscustomobject]@{Certificate=$package.Certificate;StoreName=$storeName}
                Add-DisplayCertificate -Certificate $package.Certificate -StoreName $storeName
                if (!(Test-DisplayCertificatePresent -Certificate $package.Certificate -StoreName $storeName)) { throw 'Display certificate provisioning did not persist.' }
            }
        }
        $signature = Get-AuthenticodeSignature -LiteralPath $package.CatalogPath -ErrorAction Stop
        if ($signature.Status -ne 'Valid' -or !$signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ine $package.Certificate.Thumbprint) {
            throw 'Windows still rejects the verified display catalog after publisher trust provisioning.'
        }
        Write-Host 'Verified display publisher trust is ready. Windows driver staging and load policy remain authoritative.'
    } catch {
        Undo-DisplayTrustProvisioning
        throw
    }
}


function Assert-Administrator {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '[SunshineVirtualDisplay] Administrator privileges are required to install or remove the virtual display driver.'
    }
}

function Resolve-SystemToolPath {
    param([Parameter(Mandatory = $true)][string]$ToolName)

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

function ConvertTo-ProcessArgumentString {
    param([string[]]$ArgumentList = @())

    $quoted = @()
    foreach ($argument in $ArgumentList) {
        $arg = [string]$argument
        if ($arg.Length -eq 0) {
            $quoted += '""'
            continue
        }
        if ($arg -notmatch '[\s"]') {
            $quoted += $arg
            continue
        }

        $builder = [System.Text.StringBuilder]::new()
        [void]$builder.Append('"')
        $backslashes = 0
        foreach ($ch in $arg.ToCharArray()) {
            if ($ch -eq '\') {
                $backslashes++
                continue
            }
            if ($ch -eq '"') {
                [void]$builder.Append(('\' * (($backslashes * 2) + 1)))
                [void]$builder.Append('"')
                $backslashes = 0
                continue
            }
            if ($backslashes -gt 0) {
                [void]$builder.Append(('\' * $backslashes))
                $backslashes = 0
            }
            [void]$builder.Append($ch)
        }
        if ($backslashes -gt 0) {
            [void]$builder.Append(('\' * ($backslashes * 2)))
        }
        [void]$builder.Append('"')
        $quoted += $builder.ToString()
    }

    return ($quoted -join ' ')
}

function Invoke-DriverProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [int[]]$AllowedExitCodes = @(0, 259, 3010),
        [int]$TimeoutSeconds = 300
    )

    $process = Start-Process -FilePath $FilePath -ArgumentList (ConvertTo-ProcessArgumentString -ArgumentList $ArgumentList) -WorkingDirectory $scriptDir -PassThru -WindowStyle Hidden
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        try {
            $process.Kill()
        } catch {
            $null = $_
        }
        throw "[SunshineVirtualDisplay] $FilePath timed out after $TimeoutSeconds seconds."
    }
    if ($process.ExitCode -notin $AllowedExitCodes) {
        throw "[SunshineVirtualDisplay] $FilePath failed with exit code $($process.ExitCode)."
    }
    if ($process.ExitCode -eq 3010) {
        $script:rebootRequired = $true
    }
}

function Invoke-DriverProcessCapture {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [int[]]$AllowedExitCodes = @(0),
        [int]$TimeoutSeconds = 120
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = ConvertTo-ProcessArgumentString -ArgumentList $ArgumentList
    $startInfo.WorkingDirectory = $scriptDir
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        [void]$process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()

        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try {
                $process.Kill()
            } catch {
                $null = $_
            }
            throw "[SunshineVirtualDisplay] $FilePath timed out after $TimeoutSeconds seconds."
        }

        $process.WaitForExit()
        $stdout = if ($stdoutTask.Wait(5000)) { $stdoutTask.Result } else { '' }
        $stderr = if ($stderrTask.Wait(5000)) { $stderrTask.Result } else { '' }
        $output = (@($stdout -split "`r?`n") + @($stderr -split "`r?`n")) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

        if ($process.ExitCode -notin $AllowedExitCodes) {
            $detail = ($output | ForEach-Object { [string]$_ }) -join "`n"
            throw "[SunshineVirtualDisplay] $FilePath failed with exit code $($process.ExitCode). $detail"
        }

        return @($output | ForEach-Object { [string]$_ })
    } finally {
        $process.Dispose()
    }
}

function Invoke-DriverHealthProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [int]$TimeoutSeconds = 120
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = ConvertTo-ProcessArgumentString -ArgumentList $ArgumentList
    $startInfo.WorkingDirectory = $scriptDir
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        [void]$process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()

        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try {
                $process.Kill()
            } catch {
                $null = $_
            }
            return [pscustomobject]@{
                ExitCode = -1
                Output = "$FilePath timed out after $TimeoutSeconds seconds."
            }
        }

        $process.WaitForExit()
        $stdout = if ($stdoutTask.Wait(5000)) { $stdoutTask.Result } else { '' }
        $stderr = if ($stderrTask.Wait(5000)) { $stderrTask.Result } else { '' }
        $output = (@($stdout -split "`r?`n") + @($stderr -split "`r?`n")) |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output = (($output | ForEach-Object { [string]$_ }) -join "`n")
        }
    } finally {
        $process.Dispose()
    }
}

function Assert-Artifact {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "[SunshineVirtualDisplay] Required driver artifact missing: $Path"
    }
    if ((Get-Item -LiteralPath $Path).Length -le 0) {
        throw "[SunshineVirtualDisplay] Required driver artifact is empty: $Path"
    }
}

function Assert-InfContent {
    $infText = Get-Content -LiteralPath $infPath -Raw
    foreach ($required in @(
        'Root\SunshineVirtualDisplay',
        'SunshineVirtualDisplayDriver.dll',
        'CatalogFile=SunshineVirtualDisplayDriver.cat',
        'AddInterface={5f894d6c-3a69-48a2-86ef-e4c671932d63},,ControlInterface',
        '[ControlInterface_AddReg]',
        'HKR,,Security,,"D:P(A;;GA;;;SY)(A;;GA;;;S-1-5-80-2333729190-1599198784-3320592948-2337414441-3098439965)"',
        'HKR,,"ConfigVersion",0x00010001,1',
        'UmdfExtensions=IddCx0102',
        'SunshineVirtualDisplayGroup'
    )) {
        if ($infText -notlike "*$required*") {
            throw "[SunshineVirtualDisplay] INF is missing expected content: $required"
        }
    }
}

function Assert-CatalogSignature {
    try { $null = Assert-ExpectedDisplayPackage } catch {
        if ($ValidateOnly -and $AllowUnsignedCatalogForValidation) {
            Write-Warning "[SunshineVirtualDisplay] Package validation explicitly allowed an unverified local catalog: $($_.Exception.Message)"
            return
        }
        throw
    }
}

function Assert-Package {
    foreach ($artifact in @($infPath, $dllPath, $catPath, $nefConc, $probePath)) {
        Assert-Artifact -Path $artifact
    }
    if (Test-Path -LiteralPath $certPath -PathType Leaf) {
        Assert-Artifact -Path $certPath
    }

    Assert-VulkanLayerPackage
    Assert-InfContent
    Assert-CatalogSignature
}

function Assert-VulkanLayerPackage {
    foreach ($layer in $vulkanLayers) {
        Assert-Artifact -Path $layer.Dll
        Assert-Artifact -Path $layer.Manifest
        $bytes = [System.IO.File]::ReadAllBytes($layer.Dll)
        if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
            throw "[SunshineVirtualDisplay] Invalid Vulkan layer DLL: $($layer.Dll)"
        }
        $peOffset = [System.BitConverter]::ToInt32($bytes, 0x3c)
        if ($peOffset -lt 0 -or $peOffset -gt ($bytes.Length - 6) -or
            [System.BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550 -or
            [System.BitConverter]::ToUInt16($bytes, $peOffset + 4) -ne $layer.Machine) {
            throw "[SunshineVirtualDisplay] Wrong Vulkan layer DLL architecture for $($layer.View): $($layer.Dll)"
        }
        $manifest = Get-Content -LiteralPath $layer.Manifest -Raw | ConvertFrom-Json
        $expectedLibrary = '.\' + [System.IO.Path]::GetFileName($layer.Dll)
        if ($manifest.layer.name -cne 'VK_LAYER_SUNSHINE_virtual_hdr' -or
            $manifest.layer.library_path -cne $expectedLibrary -or
            $manifest.layer.functions.vkGetInstanceProcAddr -cne 'vkGetInstanceProcAddr') {
            throw "[SunshineVirtualDisplay] Vulkan layer manifest does not match its architecture: $($layer.Manifest)"
        }
    }
}

function Open-LocalMachineRegistryKey {
    param(
        [Parameter(Mandatory = $true)]
        [Microsoft.Win32.RegistryView]$View,

        [Parameter(Mandatory = $true)]
        [string]$SubKey,

        [bool]$Writable = $false,
        [bool]$Create = $false
    )

    $baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $View)
    try {
        if ($Create) {
            return $baseKey.CreateSubKey($SubKey, $Writable)
        }

        return $baseKey.OpenSubKey($SubKey, $Writable)
    } finally {
        $baseKey.Dispose()
    }
}

function Register-VulkanLayer {
    # Check both DLL/manifest pairs before removing any working registration.
    Assert-VulkanLayerPackage
    Unregister-VulkanLayer
    foreach ($layer in $vulkanLayers) {
        $jsonFullPath = (Resolve-Path -LiteralPath $layer.Manifest).Path
        $key = Open-LocalMachineRegistryKey `
            -View $layer.View `
            -SubKey $vulkanImplicitLayersSubKey `
            -Writable $true `
            -Create $true
        if (-not $key) {
            throw "[SunshineVirtualDisplay] Unable to open HKLM:\$vulkanImplicitLayersSubKey in $($layer.View)."
        }
        try {
            $key.SetValue($jsonFullPath, 0, [Microsoft.Win32.RegistryValueKind]::DWord)
            Write-Host "[SunshineVirtualDisplay] Vulkan HDR implicit layer registered in $($layer.View): $jsonFullPath"
        } finally {
            $key.Dispose()
        }
    }
}

function Unregister-VulkanLayer {
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64, [Microsoft.Win32.RegistryView]::Registry32)) {
        $key = Open-LocalMachineRegistryKey `
            -View $view `
            -SubKey $vulkanImplicitLayersSubKey `
            -Writable $true
        if (-not $key) {
            continue
        }

        try {
            $removed = 0
            foreach ($valueName in @($key.GetValueNames())) {
                if ([System.IO.Path]::GetFileName($valueName) -in @('VkLayer_sunshine_hdr.json', 'VkLayer_sunshine_hdr_x86.json')) {
                    $key.DeleteValue($valueName, $false)
                    $removed++
                }
            }

            if ($removed -gt 0) {
                Write-Host "[SunshineVirtualDisplay] Vulkan HDR implicit layer registrations removed from ${view}: $removed"
            }
        } finally {
            $key.Dispose()
        }
    }
}

function Install-CertificateIfPresent {
    param([Parameter(Mandatory = $true)][string]$StoreName)

    if (-not (Test-Path -LiteralPath $certPath -PathType Leaf)) {
        Write-Host "[SunshineVirtualDisplay] No certificate found for LocalMachine\$StoreName; continuing."
        return
    }

    $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new([System.IO.File]::ReadAllBytes($certPath))
    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new($StoreName, 'LocalMachine')
    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $existing = $store.Certificates.Find([System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $cert.Thumbprint, $false)
        if ($existing.Count -eq 0) {
            $store.Add($cert)
            Write-Host "[SunshineVirtualDisplay] Certificate installed into LocalMachine\$StoreName."
        } else {
            Write-Host "[SunshineVirtualDisplay] Certificate already present in LocalMachine\$StoreName."
        }
    } finally {
        $store.Close()
    }
}

# Windows PnP needs a third-party driver publisher's end-entity certificate
# in LocalMachine\TrustedPublisher to install its catalog without prompting.
# Add only the expected, already-valid SignPath signer; local/test packages
# continue using the bundled certificate path above.
function Get-ExpectedCatalogSignerCertificate {
    param([string]$CatalogPath = $script:catPath)

    $signature = Get-AuthenticodeSignature -LiteralPath $CatalogPath
    $signer = $signature.SignerCertificate
    if (-not $signer -or $signature.Status -ne 'Valid') {
        return
    }
    if (-not [string]::Equals($signer.Subject, $signPathSignerSubject, [System.StringComparison]::OrdinalIgnoreCase)) {
        return
    }

    return $signer
}

function Install-CatalogSignerCertificate {
    $signer = Get-ExpectedCatalogSignerCertificate
    if (-not $signer) {
        return
    }

    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new('TrustedPublisher', 'LocalMachine')
    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $existing = $store.Certificates.Find([System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $signer.Thumbprint, $false)
        if ($existing.Count -eq 0) {
            $store.Add($signer)
            Write-Host '[SunshineVirtualDisplay] SignPath publisher certificate installed into LocalMachine\TrustedPublisher for silent driver installation.'
        } else {
            Write-Host '[SunshineVirtualDisplay] SignPath publisher certificate already present in LocalMachine\TrustedPublisher.'
        }
    } finally {
        $store.Close()
    }
}

function Remove-CertificateIfPresent {
    param([Parameter(Mandatory = $true)][string]$StoreName)

    if (-not (Test-Path -LiteralPath $certPath -PathType Leaf)) {
        return
    }

    $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new([System.IO.File]::ReadAllBytes($certPath))
    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new($StoreName, 'LocalMachine')
    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $existing = $store.Certificates.Find([System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $cert.Thumbprint, $false)
        foreach ($entry in $existing) {
            $store.Remove($entry)
            Write-Host "[SunshineVirtualDisplay] Certificate removed from LocalMachine\$StoreName."
        }
    } finally {
        $store.Close()
    }
}

function Stop-ApolloForDriverInstall {
    $service = Get-Service -Name 'ApolloService' -ErrorAction SilentlyContinue
    if ($service -and $service.Status -eq 'Running') {
        $script:apolloServiceWasRunning = $true
    }
    if ($service -and $service.Status -ne 'Stopped') {
        Write-Host '[SunshineVirtualDisplay] Stopping Apollo service before driver repair.'
        Stop-Service -Name 'ApolloService' -Force -ErrorAction Stop
        $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
    }

    foreach ($process in @(Get-Process -Name 'sunshine' -ErrorAction SilentlyContinue)) {
        Write-Host "[SunshineVirtualDisplay] Stopping Sunshine process $($process.Id) before driver repair."
        Stop-Process -Id $process.Id -Force -ErrorAction Stop
    }
}

function Start-ApolloServiceIfNeeded {
    if (-not $script:apolloServiceWasRunning) {
        return
    }

    $service = Get-Service -Name 'ApolloService' -ErrorAction SilentlyContinue
    if (-not $service) {
        $script:apolloServiceWasRunning = $false
        return
    }
    if ($service.Status -eq 'Running') {
        $script:apolloServiceWasRunning = $false
        return
    }

    Write-Host '[SunshineVirtualDisplay] Restarting Apollo service after driver repair.'
    Start-Service -Name 'ApolloService' -ErrorAction Stop
    $service.WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
    $script:apolloServiceWasRunning = $false
}

function Stop-VirtualDisplayBrokerForDriverInstall {
    $service = Get-Service -Name 'SunshineVirtualDisplayBroker' -ErrorAction SilentlyContinue
    if ($service -and $service.Status -eq 'Running') {
        $script:virtualDisplayBrokerWasRunning = $true
    }
    if ($service -and $service.Status -ne 'Stopped') {
        Write-Host '[SunshineVirtualDisplay] Stopping virtual display broker before driver repair.'
        Stop-Service -Name 'SunshineVirtualDisplayBroker' -Force -ErrorAction Stop
        $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
    }

    foreach ($process in @(Get-Process -Name 'virtualdisplay_broker' -ErrorAction SilentlyContinue)) {
        Write-Host "[SunshineVirtualDisplay] Stopping stale virtual display broker process $($process.Id)."
        Stop-Process -Id $process.Id -Force -ErrorAction Stop
    }
}

function Start-VirtualDisplayBrokerIfNeeded {
    if (-not $script:virtualDisplayBrokerWasRunning) {
        return
    }

    $service = Get-Service -Name 'SunshineVirtualDisplayBroker' -ErrorAction SilentlyContinue
    if (-not $service) {
        $script:virtualDisplayBrokerWasRunning = $false
        return
    }
    if ($service.Status -eq 'Running') {
        $script:virtualDisplayBrokerWasRunning = $false
        return
    }

    Write-Host '[SunshineVirtualDisplay] Starting virtual display broker after driver replacement.'
    Start-Service -Name 'SunshineVirtualDisplayBroker' -ErrorAction Stop
    $service.WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
    $script:virtualDisplayBrokerWasRunning = $false
}

function Stop-SunshineVirtualDisplayDriverHost {
    $hosts = @(
        Get-CimInstance Win32_Process -Filter "name='WUDFHost.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -match [regex]::Escape("-DeviceGroupId:$deviceGroupId") }
    )

    foreach ($hostProcess in $hosts) {
        Write-Host "[SunshineVirtualDisplay] Stopping stale UMDF host $($hostProcess.ProcessId) for $deviceGroupId."
        Stop-Process -Id $hostProcess.ProcessId -Force -ErrorAction SilentlyContinue
    }
}

function Restart-SunshineVirtualDisplayRuntime {
    if ($script:rebootRequired) { return }
    Stop-VirtualDisplayBrokerForDriverInstall
    Stop-SunshineVirtualDisplayDriverHost
    Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices') -AllowedExitCodes @(0, 259, 3010)
    Start-VirtualDisplayBrokerIfNeeded
}

function Install-DriverPackage {
    if ($script:rebootRequired) { return }
    Write-Host '[SunshineVirtualDisplay] Installing driver package.'
    Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/add-driver', $infPath, '/install')
}

function Grant-RegistryKeyAccess {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Identity,
        [switch]$InheritToChildKeys
    )

    $identityReference = if ($Identity -match '^S-\d-\d+(-\d+)+$') {
        [System.Security.Principal.SecurityIdentifier]::new($Identity)
    } else {
        [System.Security.Principal.NTAccount]::new($Identity)
    }
    $rights = [System.Security.AccessControl.RegistryRights]'ReadKey, WriteKey, CreateSubKey, SetValue'
    $inheritance = if ($InheritToChildKeys) {
        [System.Security.AccessControl.InheritanceFlags]::ContainerInherit
    } else {
        [System.Security.AccessControl.InheritanceFlags]::None
    }
    $rule = [System.Security.AccessControl.RegistryAccessRule]::new(
        $identityReference,
        $rights,
        $inheritance,
        [System.Security.AccessControl.PropagationFlags]::None,
        [System.Security.AccessControl.AccessControlType]::Allow
    )

    if ($Path.StartsWith('HKLM:\', [System.StringComparison]::OrdinalIgnoreCase)) {
        $subPath = $Path.Substring('HKLM:\'.Length)
        $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
            $subPath,
            [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
            [System.Security.AccessControl.RegistryRights]::ChangePermissions
        )
        if (-not $key) {
            throw "[SunshineVirtualDisplay] Registry key not found: $Path"
        }
        try {
            $acl = $key.GetAccessControl()
            $acl.SetAccessRule($rule)
            $key.SetAccessControl($acl)
        } finally {
            $key.Dispose()
        }
        return
    }

    $acl = Get-Acl -LiteralPath $Path
    $acl.SetAccessRule($rule)
    Set-Acl -LiteralPath $Path -AclObject $acl
}

function Initialize-DriverStateRegistryAccess {
    $enumRoot = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT\DISPLAY'
    if (-not (Test-Path -LiteralPath $enumRoot -PathType Container)) {
        return
    }

    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        $applied = $false
        foreach ($deviceKey in @(Get-ChildItem -LiteralPath $enumRoot -ErrorAction SilentlyContinue)) {
            $properties = Get-ItemProperty -LiteralPath $deviceKey.PSPath -ErrorAction SilentlyContinue
            if (-not $properties -or -not ($properties.HardwareID -contains $hardwareId)) {
                continue
            }

            $devicePath = 'HKLM:\' + $deviceKey.Name.Substring('HKEY_LOCAL_MACHINE\'.Length)
            $parametersPath = Join-Path $devicePath 'Device Parameters'
            if (-not (Test-Path -LiteralPath $parametersPath -PathType Container)) {
                New-Item -Path $parametersPath -Force -ErrorAction SilentlyContinue | Out-Null
            }
            if (-not (Test-Path -LiteralPath $parametersPath -PathType Container)) {
                continue
            }

            Grant-RegistryKeyAccess -Path $parametersPath -Identity $userModeDriversSid -InheritToChildKeys
            Write-Host "[SunshineVirtualDisplay] Driver state registry access is ready at $parametersPath."
            $applied = $true
        }

        if ($applied) {
            return
        }

        Start-Sleep -Milliseconds 500
    }

    throw '[SunshineVirtualDisplay] Unable to prepare driver state registry access.'
}

function Test-DeviceNodePresent {
    $enumRoot = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT\DISPLAY'
    if (-not (Test-Path -LiteralPath $enumRoot -PathType Container)) {
        return $false
    }

    foreach ($deviceKey in @(Get-ChildItem -LiteralPath $enumRoot -ErrorAction SilentlyContinue)) {
        $properties = Get-ItemProperty -LiteralPath $deviceKey.PSPath -ErrorAction SilentlyContinue
        if ($properties -and ($properties.HardwareID -contains $hardwareId)) {
            return $true
        }
    }

    return $false
}

function Add-MatchingDriverPublishedName {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$PublishedNames,
        [Parameter(Mandatory = $true)][System.Collections.Generic.HashSet[string]]$ExpectedOriginalNames,
        [string]$PublishedName,
        [string]$OriginalName
    )

    if ([string]::IsNullOrWhiteSpace($PublishedName) -or [string]::IsNullOrWhiteSpace($OriginalName)) {
        return
    }

    $publishedLeaf = [System.IO.Path]::GetFileName($PublishedName.Trim()).ToLowerInvariant()
    $originalLeaf = [System.IO.Path]::GetFileName($OriginalName.Trim())
    if (-not $ExpectedOriginalNames.Contains($originalLeaf)) {
        return
    }

    # Only names issued by Windows for third-party INF packages are safe to
    # pass to pnputil /delete-driver. Never accept a path or arbitrary token
    # from parsed command output.
    if ($publishedLeaf -notmatch '^oem[0-9]+\.inf$') {
        Write-Warning "[SunshineVirtualDisplay] Ignoring unsafe published driver name '$PublishedName'."
        return
    }

    if (-not $PublishedNames.Contains($publishedLeaf)) {
        [void]$PublishedNames.Add($publishedLeaf)
    }
}

function Get-SunshineDriverPublishedNamesFromInfCache {
    $publishedNames = [System.Collections.Generic.List[string]]::new()
    $systemRoot = if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) { 'C:\Windows' } else { $env:SystemRoot }
    $windowsInfRoot = Join-Path $systemRoot 'INF'
    if (-not (Test-Path -LiteralPath $windowsInfRoot -PathType Container)) {
        return @()
    }

    foreach ($publishedInf in @(Get-ChildItem -LiteralPath $windowsInfRoot -File -Filter 'oem*.inf' -ErrorAction SilentlyContinue)) {
        try {
            $content = [System.IO.File]::ReadAllText($publishedInf.FullName)
        } catch {
            continue
        }

        # This fallback is intentionally strict: all three package-owned
        # identifiers must match before an OEM INF can be selected for removal.
        $matchesClass = $content -match '(?im)^\s*ClassGuid\s*=\s*\{4d36e968-e325-11ce-bfc1-08002be10318\}\s*$'
        $matchesCatalog = $content -match '(?im)^\s*CatalogFile(?:\.[^=]+)?\s*=\s*SunshineVirtualDisplayDriver\.cat\s*$'
        $matchesHardware = $content -match '(?im)^\s*[^;\r\n]+\s*=\s*[^,\r\n]+,\s*Root\\SunshineVirtualDisplay\s*$'
        if ($matchesClass -and $matchesCatalog -and $matchesHardware) {
            $publishedName = $publishedInf.Name.ToLowerInvariant()
            if ($publishedName -match '^oem[0-9]+\.inf$' -and -not $publishedNames.Contains($publishedName)) {
                [void]$publishedNames.Add($publishedName)
            }
        }
    }

    return @($publishedNames)
}

function Get-DisplayDriverPublishedNamesByOriginalName {
    param([Parameter(Mandatory = $true)][string[]]$OriginalNames)

    $publishedNames = [System.Collections.Generic.List[string]]::new()
    $expectedOriginalNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $OriginalNames) {
        if (-not [string]::IsNullOrWhiteSpace($name)) {
            [void]$expectedOriginalNames.Add([System.IO.Path]::GetFileName($name.Trim()))
        }
    }

    $machineReadableEnumerationSucceeded = $false
    try {
        $xmlOutput = Invoke-DriverProcessCapture -FilePath $pnputil -ArgumentList @('/enum-drivers', '/class', $classGuid, '/format', 'xml')
        $xmlText = ($xmlOutput | ForEach-Object { [string]$_ }) -join "`n"
        if ([string]::IsNullOrWhiteSpace($xmlText)) {
            throw 'pnputil returned empty XML output.'
        }

        [xml]$document = $xmlText
        if ($null -eq $document.PnpUtil) {
            throw 'pnputil XML output did not contain the expected root element.'
        }

        foreach ($driver in @($document.PnpUtil.Driver)) {
            Add-MatchingDriverPublishedName `
                -PublishedNames $publishedNames `
                -ExpectedOriginalNames $expectedOriginalNames `
                -PublishedName ([string]$driver.DriverName) `
                -OriginalName ([string]$driver.OriginalName)
        }
        $machineReadableEnumerationSucceeded = $true
    } catch {
        Write-Warning "[SunshineVirtualDisplay] Machine-readable driver enumeration failed; using compatibility fallback: $($_.Exception.Message)"
    }

    if (-not $machineReadableEnumerationSucceeded) {
        # Older pnputil versions do not support XML. The capture helper removes
        # blank lines, so a new Published Name must also close the prior record.
        $output = Invoke-DriverProcessCapture -FilePath $pnputil -ArgumentList @('/enum-drivers', '/class', $classGuid)
        $publishedName = ''
        $originalName = ''

        foreach ($line in $output) {
            if ($line -match '^\s*Published Name\s*:\s*(.+?)\s*$') {
                Add-MatchingDriverPublishedName `
                    -PublishedNames $publishedNames `
                    -ExpectedOriginalNames $expectedOriginalNames `
                    -PublishedName $publishedName `
                    -OriginalName $originalName
                $publishedName = $Matches[1]
                $originalName = ''
            } elseif ($line -match '^\s*Original Name\s*:\s*(.+?)\s*$') {
                $originalName = $Matches[1]
            }
        }

        Add-MatchingDriverPublishedName `
            -PublishedNames $publishedNames `
            -ExpectedOriginalNames $expectedOriginalNames `
            -PublishedName $publishedName `
            -OriginalName $originalName

        # The protected Windows INF cache is a language-independent fallback
        # for legacy pnputil builds whose text labels may be localized.
        foreach ($publishedName in @(Get-SunshineDriverPublishedNamesFromInfCache)) {
            if (-not $publishedNames.Contains($publishedName)) {
                [void]$publishedNames.Add($publishedName)
            }
        }
    }

    return @($publishedNames | Select-Object -Unique)
}

function Get-SunshineDriverPublishedNames {
    Get-DisplayDriverPublishedNamesByOriginalName -OriginalNames @('SunshineVirtualDisplayDriver.inf')
}

function Get-CurrentDriverStoreDllPaths {
    $systemRoot = if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) { 'C:\Windows' } else { $env:SystemRoot }
    $driverStoreRoot = Join-Path $systemRoot 'System32\DriverStore\FileRepository'
    if (-not (Test-Path -LiteralPath $driverStoreRoot -PathType Container)) {
        return @()
    }

    return @(
        Get-ChildItem -LiteralPath $driverStoreRoot -Directory -Filter 'sunshinevirtualdisplaydriver.inf_*' -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName 'SunshineVirtualDisplayDriver.dll' } |
            Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
            Select-Object -Unique
    )
}

function Test-DriverStoreMatchesPackagedPayload {
    $currentDllPaths = @(Get-CurrentDriverStoreDllPaths)
    if ($currentDllPaths.Count -eq 0) {
        return $false
    }

    $packagedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $dllPath).Hash
    $currentHashes = @(
        $currentDllPaths |
            ForEach-Object { (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash } |
            Select-Object -Unique
    )

    return $currentHashes.Count -eq 1 -and
        [string]::Equals($currentHashes[0], $packagedHash, [System.StringComparison]::OrdinalIgnoreCase)
}

function Wait-DriverStoreMatchesPackagedPayload {
    param([int]$TimeoutSeconds = 5)

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    do {
        if (Test-DriverStoreMatchesPackagedPayload) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    } while ($stopwatch.Elapsed -lt [TimeSpan]::FromSeconds($TimeoutSeconds))

    return Test-DriverStoreMatchesPackagedPayload
}

function Write-DriverStorePayloadDiagnostics {
    $packagedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $dllPath).Hash
    Write-Host "[SunshineVirtualDisplay] Packaged driver DLL SHA256: $packagedHash"

    $currentDllPaths = @(Get-CurrentDriverStoreDllPaths)
    if ($currentDllPaths.Count -eq 0) {
        Write-Host '[SunshineVirtualDisplay] DriverStore contains no Sunshine virtual display DLL payloads.'
        return
    }

    foreach ($currentDllPath in $currentDllPaths) {
        try {
            $currentHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $currentDllPath).Hash
            Write-Host "[SunshineVirtualDisplay] DriverStore DLL SHA256: $currentHash ($currentDllPath)"
        } catch {
            Write-Warning "[SunshineVirtualDisplay] Unable to hash DriverStore payload '$currentDllPath': $($_.Exception.Message)"
        }
    }
}

function Test-DriverPackageRefreshNeeded {
    $publishedNames = @(Get-SunshineDriverPublishedNames)
    if ($publishedNames.Count -eq 0) {
        Write-Host '[SunshineVirtualDisplay] No installed Sunshine virtual display driver package was found; driver install is required.'
        return $true
    }

    $currentDllPaths = @(Get-CurrentDriverStoreDllPaths)
    if ($currentDllPaths.Count -eq 0) {
        Write-Host '[SunshineVirtualDisplay] No DriverStore Sunshine virtual display DLL was found; driver install is required.'
        return $true
    }

    $packagedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $dllPath).Hash
    $currentHashes = @(
        $currentDllPaths |
            ForEach-Object { (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash } |
            Select-Object -Unique
    )

    if ($currentHashes.Count -eq 1 -and [string]::Equals($currentHashes[0], $packagedHash, [System.StringComparison]::OrdinalIgnoreCase)) {
        Write-Host '[SunshineVirtualDisplay] Installed driver package already matches packaged driver payload; skipping driver replacement.'
        return $false
    }

    Write-Host '[SunshineVirtualDisplay] Packaged driver payload differs from the installed driver package; driver replacement is required.'
    return $true
}

function Remove-DriverPackage {
    if ((-not $Uninstall) -and $script:rebootRequired) { return }
    $publishedNames = @(Get-SunshineDriverPublishedNames)
    if ($publishedNames.Count -eq 0) {
        Write-Host '[SunshineVirtualDisplay] No Sunshine virtual display driver package was found in the driver store.'
        return
    }

    Write-Host "[SunshineVirtualDisplay] Found Sunshine driver package(s): $($publishedNames -join ', ')."
    foreach ($publishedName in $publishedNames) {
        Write-Host "[SunshineVirtualDisplay] Removing driver package $publishedName."
        Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/delete-driver', $publishedName, '/uninstall', '/force')
        if ((-not $Uninstall) -and $script:rebootRequired) { return }
    }

    # PnP package bookkeeping can settle just after pnputil exits. Retry any
    # still-registered package once before declaring the clean restage failed.
    Start-Sleep -Milliseconds 500
    $remainingNames = @(Get-SunshineDriverPublishedNames)
    foreach ($publishedName in $remainingNames) {
        Write-Host "[SunshineVirtualDisplay] Retrying removal of driver package $publishedName."
        Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/delete-driver', $publishedName, '/uninstall', '/force')
        if ((-not $Uninstall) -and $script:rebootRequired) { return }
    }
}

function Remove-DeviceNodeForHardwareId {
    param(
        [Parameter(Mandatory = $true)][string]$HardwareId,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $nefConc -PathType Leaf)) {
        Write-Host '[SunshineVirtualDisplay] nefconc.exe is missing; skipping device-node removal.'
        return
    }

    try {
        Write-Host "[SunshineVirtualDisplay] Removing $Label device node for $HardwareId."
        Invoke-DriverProcess -FilePath $nefConc -ArgumentList @('--remove-device-node', '--hardware-id', $HardwareId, '--class-guid', $classGuid)
    } catch {
        Write-Warning $_.Exception.Message
    }
}

function Remove-DeviceNode {
    Remove-DeviceNodeForHardwareId -HardwareId $hardwareId -Label 'Sunshine virtual display'
}

function Test-TemporaryVirtualDisplay {
    Write-Host '[SunshineVirtualDisplay] Running temporary display self-test.'
    $result = Invoke-DriverHealthProcess -FilePath $probePath -ArgumentList @('--self-test-temp', '1920', '1080', '60') -TimeoutSeconds 60
    if ($result.ExitCode -eq 0) {
        Write-Host '[SunshineVirtualDisplay] Temporary display self-test passed.'
        return $true
    }

    if (-not [string]::IsNullOrWhiteSpace($result.Output)) {
        Write-Warning "[SunshineVirtualDisplay] Temporary display self-test failed with exit code $($result.ExitCode): $($result.Output)"
    } else {
        Write-Warning "[SunshineVirtualDisplay] Temporary display self-test failed with exit code $($result.ExitCode)."
    }
    return $false
}

function Get-SunshineDeviceInstanceId {
    try {
        $output = Invoke-DriverProcessCapture -FilePath $pnputil -ArgumentList @('/enum-devices', '/deviceid', $hardwareId, '/deviceids', '/drivers') -AllowedExitCodes @(0) -TimeoutSeconds 120
        foreach ($line in $output) {
            if ($line -match '^\s*Instance ID\s*:\s*(.+?)\s*$') {
                return $Matches[1].Trim()
            }
        }
    } catch {
        Write-Warning $_.Exception.Message
    }

    return $null
}

function Reset-SunshineVirtualDisplayDeviceNode {
    if ($script:rebootRequired) { return $false }
    Write-Host '[SunshineVirtualDisplay] Clean-restaging the driver and device node after failed runtime revive.'

    try {
        Stop-VirtualDisplayBrokerForDriverInstall
        Remove-DeviceNode
        if ($script:rebootRequired) { return $false }
        Remove-DriverPackage
        if ($script:rebootRequired) { return $false }
        Install-DriverPackage
        if ($script:rebootRequired) { return $false }
        if (-not (Wait-DriverStoreMatchesPackagedPayload -TimeoutSeconds 10)) {
            Write-DriverStorePayloadDiagnostics
            throw '[SunshineVirtualDisplay] DriverStore payload did not match the packaged driver after recovery restaging.'
        }

        Invoke-DriverProcess -FilePath $nefConc -ArgumentList @('--create-device-node', '--class-name', 'Display', '--class-guid', $classGuid, '--hardware-id', $hardwareId)
        if ($script:rebootRequired) { return $false }
        Install-DriverPackage
        if ($script:rebootRequired) { return $false }
        Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices') -AllowedExitCodes @(0, 259, 3010)
        if ($script:rebootRequired) { return $false }
        Initialize-DriverStateRegistryAccess
        Start-VirtualDisplayBrokerIfNeeded

        if (Test-TemporaryVirtualDisplay) {
            Write-Host '[SunshineVirtualDisplay] Clean driver and device-node restaging restored the virtual display driver without a restart.'
            return $true
        }
    } catch {
        Write-Warning "[SunshineVirtualDisplay] Clean driver and device-node restaging failed: $($_.Exception.Message)"
    } finally {
        try {
            Start-VirtualDisplayBrokerIfNeeded
        } catch {
            Write-Warning $_.Exception.Message
        }
    }

    return $false
}

function Invoke-InstallerHealthCheck {
    # A successful old driver can answer the probe while a replacement is still
    # pending reboot. Health observations cannot cancel a native 3010 result.
    if ($script:rebootRequired) { return }
    if (Test-TemporaryVirtualDisplay) {
        return
    }

    $instanceId = Get-SunshineDeviceInstanceId
    if ([string]::IsNullOrWhiteSpace($instanceId)) {
        Write-Warning "[SunshineVirtualDisplay] Unable to resolve device instance ID for $hardwareId; attempting clean restaging."
    } else {
        Write-Host "[SunshineVirtualDisplay] Restarting device instance after failed temporary display self-test: $instanceId"
        $restart = Invoke-DriverHealthProcess -FilePath $pnputil -ArgumentList @('/restart-device', $instanceId) -TimeoutSeconds 120
        if ($restart.ExitCode -eq 3010) {
            $script:rebootRequired = $true
            return
        } elseif ($restart.ExitCode -ne 0) {
            if (-not [string]::IsNullOrWhiteSpace($restart.Output)) {
                Write-Warning "[SunshineVirtualDisplay] pnputil restart-device failed with exit code $($restart.ExitCode): $($restart.Output)"
            } else {
                Write-Warning "[SunshineVirtualDisplay] pnputil restart-device failed with exit code $($restart.ExitCode)."
            }
        }

        try {
            Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices') -AllowedExitCodes @(0, 259, 3010)
        } catch {
            Write-Warning $_.Exception.Message
        }
        if ($script:rebootRequired) { return }
        if (Test-TemporaryVirtualDisplay) {
            return
        }

        Write-Host "[SunshineVirtualDisplay] Disabling/enabling device instance after failed restart revive: $instanceId"
        $disable = Invoke-DriverHealthProcess -FilePath $pnputil -ArgumentList @('/disable-device', $instanceId, '/force') -TimeoutSeconds 120
        if ($disable.ExitCode -eq 3010) {
            $script:rebootRequired = $true
            return
        } elseif ($disable.ExitCode -ne 0) {
            if (-not [string]::IsNullOrWhiteSpace($disable.Output)) {
                Write-Warning "[SunshineVirtualDisplay] pnputil disable-device failed with exit code $($disable.ExitCode): $($disable.Output)"
            } else {
                Write-Warning "[SunshineVirtualDisplay] pnputil disable-device failed with exit code $($disable.ExitCode)."
            }
        }

        $enable = Invoke-DriverHealthProcess -FilePath $pnputil -ArgumentList @('/enable-device', $instanceId) -TimeoutSeconds 120
        if ($enable.ExitCode -eq 3010) {
            $script:rebootRequired = $true
            return
        } elseif ($enable.ExitCode -ne 0) {
            if (-not [string]::IsNullOrWhiteSpace($enable.Output)) {
                Write-Warning "[SunshineVirtualDisplay] pnputil enable-device failed with exit code $($enable.ExitCode): $($enable.Output)"
            } else {
                Write-Warning "[SunshineVirtualDisplay] pnputil enable-device failed with exit code $($enable.ExitCode)."
            }
        }

        try {
            Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices') -AllowedExitCodes @(0, 259, 3010)
        } catch {
            Write-Warning $_.Exception.Message
        }
        if ($script:rebootRequired) { return }
        if (Test-TemporaryVirtualDisplay) {
            return
        }

        Write-Host '[SunshineVirtualDisplay] Recycling virtual display broker and UMDF host after failed PnP revive.'
        try {
            Restart-SunshineVirtualDisplayRuntime
        } catch {
            Write-Warning $_.Exception.Message
        }
        if ($script:rebootRequired) { return }
        if (Test-TemporaryVirtualDisplay) {
            return
        }
    }

    if (Reset-SunshineVirtualDisplayDeviceNode) {
        return
    }

    $script:rebootRequired = $true
}

function Install-SunshineVirtualDisplayDriver {
    # Once Windows requests a reboot, only the common completion/service restore
    # path may run. Do not diagnose an old active payload as a failed replacement.
    if ($script:rebootRequired) { return }
    $driverPackageRefreshNeeded = Test-DriverPackageRefreshNeeded
    $deviceNodePresent = Test-DeviceNodePresent
    Stop-ApolloForDriverInstall
    Stop-VirtualDisplayBrokerForDriverInstall

    if ((-not $driverPackageRefreshNeeded) -and $deviceNodePresent) {
        Initialize-DriverStateRegistryAccess
        Invoke-InstallerHealthCheck
        return
    }

    Install-DriverPackage
    if ($script:rebootRequired) { return }

    if (-not (Wait-DriverStoreMatchesPackagedPayload)) {
        Write-Warning '[SunshineVirtualDisplay] Windows retained stale or mixed Sunshine DriverStore payloads; removing Sunshine driver packages before restaging.'
        Write-DriverStorePayloadDiagnostics
        Remove-DeviceNode
        if ($script:rebootRequired) { return }
        Remove-DriverPackage
        if ($script:rebootRequired) { return }
        Install-DriverPackage
        if ($script:rebootRequired) { return }
        if (-not (Wait-DriverStoreMatchesPackagedPayload -TimeoutSeconds 10)) {
            Write-DriverStorePayloadDiagnostics
            throw '[SunshineVirtualDisplay] DriverStore payload still does not match the packaged driver after clean restaging.'
        }
        Write-Host '[SunshineVirtualDisplay] Clean DriverStore restaging replaced the stale driver payload.'
    }

    if (-not (Test-DeviceNodePresent)) {
        Write-Host '[SunshineVirtualDisplay] Creating device node.'
        Invoke-DriverProcess -FilePath $nefConc -ArgumentList @('--create-device-node', '--class-name', 'Display', '--class-guid', $classGuid, '--hardware-id', $hardwareId)
        if ($script:rebootRequired) { return }
        Install-DriverPackage
        if ($script:rebootRequired) { return }
    }

    Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices')
    if ($script:rebootRequired) { return }
    Restart-SunshineVirtualDisplayRuntime
    if ($script:rebootRequired) { return }
    Initialize-DriverStateRegistryAccess
    Invoke-InstallerHealthCheck
}

if ($RegisterVulkanLayerOnly -and $UnregisterVulkanLayerOnly) {
    throw '[SunshineVirtualDisplay] RegisterVulkanLayerOnly and UnregisterVulkanLayerOnly cannot be used together.'
}

if ($RegisterVulkanLayerOnly) {
    Assert-VulkanLayerPackage
    if ($ValidateOnly) {
        Write-Host '[SunshineVirtualDisplay] Vulkan HDR layer package validated.'
        exit 0
    }

    Assert-Administrator
    Register-VulkanLayer
    Write-Host '[SunshineVirtualDisplay] Vulkan HDR layer registration complete.'
    exit 0
}

if ($UnregisterVulkanLayerOnly) {
    if ($ValidateOnly) {
        Write-Host '[SunshineVirtualDisplay] Vulkan HDR layer unregister action validated.'
        exit 0
    }

    Assert-Administrator
    Unregister-VulkanLayer
    Write-Host '[SunshineVirtualDisplay] Vulkan HDR layer unregister complete.'
    exit 0
}

if (-not $Uninstall -and -not $ValidateOnly) {
    try {
        if (-not (Test-SafeDriverInstall -InfPath $infPath -CatalogPath $catPath -HardwareId $hardwareId)) { exit 0 }
    } catch {
        # No service or driver mutation has occurred when the guard rejects.
        if ($InstallerBestEffort) {
            Write-Warning "[SunshineVirtualDisplay] VIRTUAL_DISPLAY_DRIVER_WARNING: Optional driver update skipped; driver state unchanged; virtual display may be unavailable. $($_.Exception.Message)"
            exit 0
        }
        throw
    }
}

Assert-Package

$pnputil = Resolve-SystemToolPath -ToolName 'pnputil.exe'

if ($ValidateOnly) {
    Write-Host '[SunshineVirtualDisplay] Driver installer package validated.'
    exit 0
}

Assert-Administrator

if ($Uninstall) {
    Write-Host '[SunshineVirtualDisplay] Removing device node.'
    # Let PnP removal unload the UMDF host. Forcing WUDFHost.exe closed records
    # a critical user-mode driver crash event even when the install succeeds.
    Unregister-VulkanLayer
    Remove-DeviceNode
    Remove-DriverPackage
    Remove-CertificateIfPresent -StoreName 'TrustedPublisher'
    Remove-CertificateIfPresent -StoreName 'Root'
    Write-Host '[SunshineVirtualDisplay] Uninstall complete.'
    exit 0
}

Install-ExpectedDisplayTrust
# Verify catalog membership through Windows before service stops or device cleanup.
try {
    Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/add-driver', $infPath) -AllowedExitCodes @(0, 3010)
} catch {
    Undo-DisplayTrustProvisioning
    throw
}
Register-VulkanLayer

Install-SunshineVirtualDisplayDriver
Start-VirtualDisplayBrokerIfNeeded
Start-ApolloServiceIfNeeded

Write-Host '[SunshineVirtualDisplay] Driver install complete.'
if ($script:rebootRequired) {
    Write-DriverRestartRequired
    exit 0
}
