param(
    [switch]$Uninstall,
    [switch]$InstallerBestEffort
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $PSCommandPath
$hardwarePrefix = 'ROOT\SUDOMAKER\SUDOVDA'
$hardwareId = $hardwarePrefix.ToLowerInvariant()
$classGuid = '{4D36E968-E325-11CE-BFC1-08002BE10318}'
$nefConc = Join-Path $scriptDir 'nefconc.exe'
$infPath = Join-Path $scriptDir 'SudoVDA.inf'
$certPath = Join-Path $scriptDir 'sudovda.cer'
$catPath = Join-Path $scriptDir 'sudovda.cat'
$dllPath = Join-Path $scriptDir 'SudoVDA.dll'
$script:rebootRequired = $false
$script:driverProbeTimedOut = $false
$driverProbeTimeoutSeconds = 15
$driverStepTimeoutSeconds = 120

trap {
    Write-Warning "[SudoVDA] VIRTUAL_DISPLAY_DRIVER_WARNING: Display driver setup did not complete. $($_.Exception.Message)"
    throw
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


function Resolve-SystemToolPath {
    param([Parameter(Mandatory = $true)][string]$ToolName)

    $systemRoot = if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) { 'C:\Windows' } else { $env:SystemRoot }
    $candidates = @(
        (Join-Path -Path $systemRoot -ChildPath ("Sysnative\" + $ToolName))
        (Join-Path -Path $systemRoot -ChildPath ("System32\" + $ToolName))
        (Join-Path -Path $systemRoot -ChildPath ("SysWOW64\" + $ToolName))
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -Path $candidate -PathType Leaf) {
            return $candidate
        }
    }

    return $candidates[1]
}

$pnputil = Resolve-SystemToolPath -ToolName 'pnputil.exe'

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

        $builder = New-Object System.Text.StringBuilder
        [void]$builder.Append('"')
        $backslashes = 0
        foreach ($ch in $arg.ToCharArray()) {
            if ($ch -eq '\') {
                $backslashes++
                continue
            }
            if ($ch -eq '"') {
                if ($backslashes -gt 0) {
                    [void]$builder.Append(('\' * ($backslashes * 2)))
                    $backslashes = 0
                }
                [void]$builder.Append('\"')
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

function Invoke-Process {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$WorkingDirectory = $scriptDir,
        [int]$TimeoutSeconds = 0
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = ConvertTo-ProcessArgumentString -ArgumentList $ArgumentList
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    $timedOut = $false

    try {
        [void]$process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()

        if ($TimeoutSeconds -gt 0) {
            if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
                $timedOut = $true
                try {
                    $process.Kill()
                    $process.WaitForExit(5000) | Out-Null
                } catch {
                    $null = $_
                }
            } else {
                # Ensure async output readers have finished flushing.
                $process.WaitForExit()
            }
        } else {
            $process.WaitForExit()
        }

        $stdout = ''
        $stderr = ''
        try {
            if ($stdoutTask.Wait(5000)) { $stdout = $stdoutTask.Result }
        } catch {
            $stdout = ''
        }
        try {
            if ($stderrTask.Wait(5000)) { $stderr = $stderrTask.Result }
        } catch {
            $stderr = ''
        }

        $exitCode = 1460
        if (-not $timedOut) {
            try {
                $exitCode = $process.ExitCode
            } catch {
                $exitCode = 1
            }
        }

        return [pscustomobject]@{
            ExitCode = $exitCode
            StdOut   = $stdout
            StdErr   = $stderr
            TimedOut = $timedOut
        }
    }
    finally {
        if ($process) {
            $process.Dispose()
        }
    }
}

function Invoke-DriverStep {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$Description
    )

    $result = Invoke-Process -FilePath $FilePath -ArgumentList $ArgumentList -TimeoutSeconds $driverStepTimeoutSeconds

    if ($result.StdOut) {
        Write-Host $result.StdOut.TrimEnd()
    }
    if ($result.StdErr) {
        Write-Host $result.StdErr.TrimEnd()
    }

    if ($result.TimedOut) {
        throw "[SudoVDA] $Description timed out after $driverStepTimeoutSeconds seconds."
    }

    switch ($result.ExitCode) {
        0     { return }
        3010  { $script:rebootRequired = $true; return }
        default {
            throw "[SudoVDA] $Description failed with exit code $($result.ExitCode)."
        }
    }
}

function Write-ProcessOutput {
    param([Parameter(Mandatory = $true)]$Result)

    if ($Result.StdOut) {
        Write-Host $Result.StdOut.TrimEnd()
    }
    if ($Result.StdErr) {
        Write-Host $Result.StdErr.TrimEnd()
    }
}

function Assert-RequiredDriverArtifact {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$DisplayName
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "[SudoVDA] Required driver artifact missing: $DisplayName ($Path)"
    }

    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($item.Length -le 0) {
        throw "[SudoVDA] Required driver artifact is empty (0 bytes): $DisplayName ($Path)"
    }
}

function Assert-RequiredInstallArtifacts {
    Assert-RequiredDriverArtifact -Path $nefConc -DisplayName 'nefconc.exe'
    Assert-RequiredDriverArtifact -Path $infPath -DisplayName 'SudoVDA.inf'
    Assert-RequiredDriverArtifact -Path $dllPath -DisplayName 'SudoVDA.dll'
    Assert-RequiredDriverArtifact -Path $catPath -DisplayName 'sudovda.cat'
    Assert-RequiredDriverArtifact -Path $certPath -DisplayName 'sudovda.cer'
}

function Install-Certificate {
    param(
        [Parameter(Mandatory = $true)][string]$StoreName,
        [string]$StoreLocation = 'LocalMachine'
    )

    Assert-RequiredDriverArtifact -Path $certPath -DisplayName 'sudovda.cer'

    try {
        $certBytes = [System.IO.File]::ReadAllBytes($certPath)
        $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certBytes)
    }
    catch {
        throw "[SudoVDA] Failed to load certificate from $certPath. $($_.Exception.Message)"
    }

    if ([string]::IsNullOrWhiteSpace($cert.Thumbprint)) {
        throw "[SudoVDA] Certificate at $certPath is invalid (missing thumbprint)."
    }

    $location = [System.Enum]::Parse([System.Security.Cryptography.X509Certificates.StoreLocation], $StoreLocation, $true)
    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new($StoreName, $location)

    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $existing = $store.Certificates.Find([System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $cert.Thumbprint, $false)

        if ($existing.Count -gt 0) {
            Write-Host "[SudoVDA] Certificate already present in $StoreLocation\$StoreName."
            return
        }

        $store.Add($cert)
        Write-Host "[SudoVDA] Certificate installed into $StoreLocation\$StoreName."
    }
    catch {
        throw "[SudoVDA] Failed to install certificate into $StoreLocation\$StoreName. $($_.Exception.Message)"
    }
    finally {
        $store.Close()
    }
}

function Assert-CertificateMatchesCatalog {
    $null = Assert-ExpectedDisplayPackage
}

function Get-TargetDriverVersion {
    try {
        $content = Get-Content -Path $infPath -ErrorAction Stop
    } catch {
        throw '[SudoVDA] Unable to read SudoVDA INF for version check.'
    }

    foreach ($line in $content) {
        if ($line -match '^\s*DriverVer\s*=\s*[^,]+,\s*([0-9\.]+)') {
            return $matches[1].Trim()
        }
    }

    return $null
}

function Get-PresentSudoVdaDevices {
    try {
        $result = Invoke-Process -FilePath $pnputil -ArgumentList @('/enum-devices', '/class', 'Display', '/connected') -TimeoutSeconds $driverProbeTimeoutSeconds
        if ($result.TimedOut) {
            $script:driverProbeTimedOut = $true
            Write-Warning "[SudoVDA] Timed out while enumerating display devices with pnputil after $driverProbeTimeoutSeconds seconds."
            return @()
        }
        if ($result.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($result.StdOut)) {
            return @()
        }

        $entries = @()
        $current = @{}
        foreach ($line in ($result.StdOut -split "`r?`n")) {
            if ($line -match '^\s*Instance ID\s*:\s*(.+)$') {
                $current['InstanceId'] = $matches[1].Trim()
            }
            elseif ($line -match '^\s*Device Description\s*:\s*(.+)$') {
                $current['DeviceDescription'] = $matches[1].Trim()
            }
            elseif ($line -match '^\s*Manufacturer Name\s*:\s*(.+)$') {
                $current['ManufacturerName'] = $matches[1].Trim()
            }
            elseif ($line -match '^\s*Status\s*:\s*(.+)$') {
                $current['Status'] = $matches[1].Trim()
            }
            elseif ($line -match '^\s*Driver Name\s*:\s*(.+)$') {
                $current['DriverName'] = $matches[1].Trim()
            }
            elseif ($line -match '^\s*$') {
                if ($current.ContainsKey('InstanceId')) {
                    $entries += [pscustomobject]$current
                }
                $current = @{}
            }
        }

        if ($current.ContainsKey('InstanceId')) {
            $entries += [pscustomobject]$current
        }

        return $entries | Where-Object {
            $_.ManufacturerName -like "*SudoMaker*" -or
            $_.DeviceDescription -like "*SudoMaker*" -or
            $_.DeviceDescription -like "*SudoVDA*"
        }
    } catch {
        return @()
    }
}

function Get-InstalledDriverInfo {
    try {
        # Avoid Win32_PnPSignedDriver/Get-PnpDevice here. They can block indefinitely
        # under Windows Installer's hidden SYSTEM PowerShell host and stall the MSI.
        # pnputil returns quickly in the same environment and gives us the driver
        # store version needed to decide whether we can skip reinstalling SudoVDA.
        $driverPackages = @(Get-InstalledDriverPackages -Quiet)
        if ($driverPackages.Count -gt 0) {
            $driverPackage = $driverPackages |
                Sort-Object -Property @{ Expression = { Convert-Version -Version $_.DriverVersion }; Descending = $true }, PublishedName |
                Select-Object -First 1

            return [pscustomobject]@{
                DeviceName     = 'SudoMaker Virtual Display Adapter'
                Manufacturer   = $driverPackage.ProviderName
                DriverVersion  = $driverPackage.DriverVersion
                DeviceID       = $driverPackage.PublishedName
                PublishedName  = $driverPackage.PublishedName
            }
        }

        $devices = @(Get-PresentSudoVdaDevices)
        if ($devices.Count -gt 0) {
            $device = $devices | Select-Object -First 1
            return [pscustomobject]@{
                DeviceName     = $device.DeviceDescription
                Manufacturer   = $device.ManufacturerName
                DriverVersion  = $null
                DeviceID       = $device.InstanceId
                PublishedName  = $device.DriverName
            }
        }

        return $null
    } catch {
        return $null
    }
}

function Convert-Version {
    param([string]$Version)

    if (-not $Version) {
        return $null
    }

    try {
        return [version]$Version
    } catch {
        return $null
    }
}

function Test-DriverPresent {
    # Only check present devices to avoid detecting ghost/phantom entries
    # from previous installations that are no longer functional. Use pnputil
    # instead of Get-PnpDevice because PowerShell's PnP cmdlets can hang under MSI.
    try {
        $devices = @(Get-PresentSudoVdaDevices)
        if ($devices.Count -gt 0) {
            return $true
        }
    } catch {
        $null = $_
    }

    return $false
}

function Get-InstalledDriverPackages {
    param([switch]$Quiet)

    $result = Invoke-Process -FilePath $pnputil -ArgumentList @('/enum-drivers') -TimeoutSeconds $driverProbeTimeoutSeconds
    if (-not $Quiet) {
        Write-ProcessOutput -Result $result
    }

    if ($result.TimedOut) {
        $script:driverProbeTimedOut = $true
        Write-Warning "[SudoVDA] Timed out while enumerating driver packages with pnputil after $driverProbeTimeoutSeconds seconds."
        return @()
    }

    if ($result.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($result.StdOut)) {
        return @()
    }

    $entries = @()
    $current = @{}
    foreach ($line in ($result.StdOut -split "`r?`n")) {
        if ($line -match '^\s*Published Name\s*:\s*(\S+)') {
            $current['PublishedName'] = $matches[1]
        }
        elseif ($line -match '^\s*Original Name\s*:\s*(.+)$') {
            $current['OriginalName'] = $matches[1].Trim()
        }
        elseif ($line -match '^\s*Provider Name\s*:\s*(.+)$') {
            $current['ProviderName'] = $matches[1].Trim()
        }
        elseif ($line -match '^\s*Driver Version\s*:\s*(.+)$') {
            $driverVersionText = $matches[1].Trim()
            $current['DriverVersionText'] = $driverVersionText
            if ($driverVersionText -match '^\S+\s+([0-9]+(?:\.[0-9]+)+)\s*$') {
                $current['DriverVersion'] = $matches[1].Trim()
            } else {
                $current['DriverVersion'] = $driverVersionText
            }
        }
        elseif ($line -match '^\s*$') {
            if ($current.ContainsKey('PublishedName')) {
                $entries += [pscustomobject]$current
            }
            $current = @{}
        }
    }

    if ($current.ContainsKey('PublishedName')) {
        $entries += [pscustomobject]$current
    }

    return $entries | Where-Object {
        $_.OriginalName -match '^SudoVDA\.inf$' -or $_.ProviderName -match 'SudoMaker'
    }
}

function Remove-DriverPackage {
    param([Parameter(Mandatory = $true)][string]$PublishedName)

    Write-Host "[SudoVDA] Removing driver package $PublishedName."
    $result = Invoke-Process -FilePath $pnputil -ArgumentList @('/delete-driver', $PublishedName, '/uninstall', '/force') -TimeoutSeconds $driverStepTimeoutSeconds
    Write-ProcessOutput -Result $result
    if ($result.TimedOut) {
        throw "[SudoVDA] Timed out while removing driver package $PublishedName after $driverStepTimeoutSeconds seconds."
    }

    switch ($result.ExitCode) {
        0     { return }
        3010  { $script:rebootRequired = $true; return }
        default { throw "[SudoVDA] Failed to remove driver package $PublishedName (exit code $($result.ExitCode))." }
    }
}

function Invoke-SudoVdaUninstall {
    if (-not (Test-Path -Path $pnputil -PathType Leaf)) {
        throw '[SudoVDA] Unable to locate pnputil.exe.'
    }

    if (Test-Path -Path $nefConc -PathType Leaf) {
        Write-Host '[SudoVDA] Removing existing SudoVDA device node.'
        $removeResult = Invoke-Process -FilePath $nefConc -ArgumentList @('--remove-device-node', '--hardware-id', $hardwareId, '--class-guid', $classGuid) -TimeoutSeconds $driverStepTimeoutSeconds
        Write-ProcessOutput -Result $removeResult
        switch ($removeResult.ExitCode) {
            0     { }
            3010  { $script:rebootRequired = $true }
            1460  { Write-Warning "[SudoVDA] Remove-device-node timed out after $driverStepTimeoutSeconds seconds. Continuing." }
            default { Write-Warning "[SudoVDA] Remove-device-node returned exit code $($removeResult.ExitCode). Continuing." }
        }
    } else {
        Write-Warning '[SudoVDA] nefconc.exe not found; skipping device-node removal.'
    }

    $driverPackages = @(Get-InstalledDriverPackages)
    if ($driverPackages.Count -eq 0) {
        Write-Host '[SudoVDA] No matching installed driver package found; trying direct INF removal.'
        $fallback = Invoke-Process -FilePath $pnputil -ArgumentList @('/delete-driver', 'SudoVDA.inf', '/uninstall', '/force') -TimeoutSeconds $driverStepTimeoutSeconds
        Write-ProcessOutput -Result $fallback
        switch ($fallback.ExitCode) {
            0     { return }
            3010  { $script:rebootRequired = $true; return }
            1460  { throw "[SudoVDA] Direct INF removal timed out after $driverStepTimeoutSeconds seconds." }
            default {
                if (Test-DriverPresent) {
                    throw "[SudoVDA] Direct INF removal failed (exit code $($fallback.ExitCode)) and the driver is still present."
                }
                Write-Host '[SudoVDA] Driver package was already absent.'
                return
            }
        }
    }

    foreach ($driverPackage in $driverPackages) {
        Remove-DriverPackage -PublishedName $driverPackage.PublishedName
    }
}

if ($Uninstall) {
    Invoke-SudoVdaUninstall
    Write-Host '[SudoVDA] Driver uninstall complete.'
    if ($script:rebootRequired) {
        Write-Host '[SudoVDA] A reboot is required to finalize driver removal.'
    }
    $global:LastExitCode = 0
    exit 0
}

try {
    if (-not (Test-SafeDriverInstall -InfPath $infPath -CatalogPath $catPath -HardwareId $hardwareId)) { exit 0 }
} catch {
    # The read-only guard rejected the candidate before any driver changes.
    # MSI treats display setup as optional; direct use remains strict.
    if ($InstallerBestEffort) {
        Write-Warning "[SudoVDA] VIRTUAL_DISPLAY_DRIVER_WARNING: Optional driver update skipped; driver state unchanged; virtual display may be unavailable. $($_.Exception.Message)"
        exit 0
    }
    throw
}

Assert-RequiredInstallArtifacts
Assert-CertificateMatchesCatalog

$targetVersion = Get-TargetDriverVersion
$installedInfo = Get-InstalledDriverInfo
$installedVersion = if ($installedInfo) { $installedInfo.DriverVersion } else { $null }
$installedVersionObj = Convert-Version -Version $installedVersion
$targetVersionObj = Convert-Version -Version $targetVersion

Write-Host "[SudoVDA] Target version: $targetVersion"
Write-Host "[SudoVDA] Installed version: $installedVersion"
Write-Host "[SudoVDA] Driver info found: $($null -ne $installedInfo)"

if ($script:driverProbeTimedOut) {
    Write-Host '[SudoVDA] Driver probe timed out; skipping driver changes to avoid stalling or disrupting a working adapter.'
    exit 0
}

if ($installedInfo -and $installedVersionObj -and $targetVersionObj -and $installedVersionObj -ge $targetVersionObj) {
    Write-Host "[SudoVDA] Driver version $installedVersion already installed; skipping."
    exit 0
}

if ($installedInfo -and (-not $installedVersionObj -or -not $targetVersionObj)) {
    if (Test-DriverPresent) {
        Write-Host "[SudoVDA] Driver present but version info incomplete (installed=$installedVersion, target=$targetVersion); skipping to avoid disrupting a working driver."
        exit 0
    }
    Write-Host "[SudoVDA] Driver info found but device not present (installed=$installedVersion, target=$targetVersion); reinstalling."
}

if (-not $installedInfo -and (Test-DriverPresent)) {
    Write-Host '[SudoVDA] Driver detected via PnP but driver info unavailable; skipping to avoid disrupting a working driver.'
    exit 0
}

if ($installedInfo -and $installedVersion -and $targetVersion) {
    Write-Host "[SudoVDA] Upgrading driver from version $installedVersion to $targetVersion."
}


Install-ExpectedDisplayTrust

# Let Windows validate INF/DLL catalog membership before removing any existing node.
try {
    Invoke-DriverStep -FilePath $pnputil -ArgumentList @('/add-driver', $infPath) -Description 'Validate and stage SudoVDA catalog members'
} catch {
    Undo-DisplayTrustProvisioning
    throw
}

Write-Host '[SudoVDA] Removing any existing SudoVDA driver.'
Invoke-DriverStep -FilePath $nefConc -ArgumentList @('--remove-device-node', '--hardware-id', $hardwareId, '--class-guid', $classGuid) -Description 'Remove SudoVDA device node'

Write-Host '[SudoVDA] Creating virtual display device.'
Invoke-DriverStep -FilePath $nefConc -ArgumentList @('--create-device-node', '--class-name', 'Display', '--class-guid', $classGuid, '--hardware-id', $hardwareId) -Description 'Create SudoVDA device node'

Write-Host '[SudoVDA] Installing SudoVDA driver.'
Invoke-DriverStep -FilePath $nefConc -ArgumentList @('--install-driver', '--inf-path', 'SudoVDA.inf') -Description 'Install SudoVDA driver'

Write-Host '[SudoVDA] Driver install complete.'
if ($script:rebootRequired) {
    Write-Host '[SudoVDA] A reboot is required to finalize driver installation.'
}

$global:LastExitCode = 0
exit 0
