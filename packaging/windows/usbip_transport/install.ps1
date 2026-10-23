# Installs only the optional USB/IP transport. Shared existing installations
# are reused unchanged; final Vibepollo uninstall never removes these drivers.
param([switch]$InstallerBestEffort, [switch]$ValidateOnly)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package.ps1')

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

function Write-UsbIpWarning([string]$Message) {
    Write-Output "[VibepolloUsbIp] USBIP_TRANSPORT_WARNING: $Message"
}

try {
    if (![Environment]::Is64BitOperatingSystem -or ![Environment]::Is64BitProcess -or
        $env:PROCESSOR_ARCHITECTURE -ne 'AMD64' -or [Environment]::OSVersion.Version.Build -lt 18362) {
        throw 'The USB/IP haptics transport requires Windows 10 x64 version 1903 or later.'
    }
    Assert-UsbIpPackage $PSScriptRoot
    if ($ValidateOnly) {
        Write-Output '[VibepolloUsbIp] Original Microsoft-signed USB/IP package verified.'
        exit 0
    }
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Administrator privileges are required to install the USB/IP haptics transport.'
    }

    # SetupAPI implements only the root-device registration performed by the
    # upstream devnode utility. No test certificates or unsigned kernel code.
    Add-DriverInteropType -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
public static class VibepolloUsbIpSetup {
    static readonly Guid UsbClass = new Guid("36fc9e60-c465-11cf-8056-444553540000");
    static readonly Guid TransportInterface = new Guid("b4030c06-dc5f-4fcc-87eb-e5515a0935c0");
    const string HardwareId = @"ROOT\USBIP_WIN2\UDE";
    [StructLayout(LayoutKind.Sequential)] struct DeviceInfo {
        public uint Size; public Guid Class; public uint DevInst; public IntPtr Reserved;
    }
    [StructLayout(LayoutKind.Sequential)] struct InterfaceInfo {
        public uint Size; public Guid Class; public uint Flags; public IntPtr Reserved;
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct InstallParams {
        public uint Size, Flags, FlagsEx;
        public IntPtr Parent, MessageHandler, MessageContext, FileQueue, ClassReserved;
        public uint Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=260)] public string DriverPath;
    }
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr SetupDiGetClassDevsW(ref Guid guid, string enumerator, IntPtr parent, uint flags);
    [DllImport("setupapi.dll", SetLastError=true)]
    static extern bool SetupDiEnumDeviceInfo(IntPtr set, uint index, ref DeviceInfo data);
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr set, ref DeviceInfo data, uint property, out uint type, byte[] buffer, uint length, out uint required);
    [DllImport("setupapi.dll", SetLastError=true)]
    static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr device, ref Guid guid, uint index, ref InterfaceInfo data);
    [DllImport("setupapi.dll", SetLastError=true)]
    static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid guid, IntPtr parent);
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool SetupDiCreateDeviceInfoW(IntPtr set, string name, ref Guid guid, string description, IntPtr parent, uint flags, ref DeviceInfo data);
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref DeviceInfo data, uint property, byte[] buffer, uint length);
    [DllImport("setupapi.dll", SetLastError=true)]
    static extern bool SetupDiCallClassInstaller(uint function, IntPtr set, ref DeviceInfo data);
    [DllImport("setupapi.dll", SetLastError=true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool SetupDiGetDeviceInstallParamsW(IntPtr set, ref DeviceInfo data, ref InstallParams parameters);
    [DllImport("newdev.dll", ExactSpelling=true, SetLastError=true)]
    static extern bool DiInstallDevice(IntPtr parent, IntPtr set, ref DeviceInfo data, IntPtr driver, uint flags, out bool reboot);
    static void Check(bool ok) { if (!ok) throw new Win32Exception(Marshal.GetLastWin32Error()); }
    static void CheckSet(IntPtr set) { if (set == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error()); }
    public static bool Available() {
        Guid guid = TransportInterface;
        IntPtr set = SetupDiGetClassDevsW(ref guid, null, IntPtr.Zero, 0x12); // PRESENT | DEVICEINTERFACE
        CheckSet(set);
        try {
            var data = new InterfaceInfo { Size=(uint)Marshal.SizeOf(typeof(InterfaceInfo)) };
            return SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref guid, 0, ref data);
        } finally { SetupDiDestroyDeviceInfoList(set); }
    }
    public static bool HasRootDevice() {
        Guid guid = UsbClass;
        IntPtr set = SetupDiGetClassDevsW(ref guid, "ROOT", IntPtr.Zero, 4); // ALLCLASSES, includes disabled devices
        CheckSet(set);
        try {
            var data = new DeviceInfo { Size=(uint)Marshal.SizeOf(typeof(DeviceInfo)) };
            for (uint i=0; SetupDiEnumDeviceInfo(set, i, ref data); ++i) {
                byte[] bytes = new byte[16384]; uint type, needed;
                if (SetupDiGetDeviceRegistryPropertyW(set, ref data, 1, out type, bytes, (uint)bytes.Length, out needed)) {
                    foreach (string id in Encoding.Unicode.GetString(bytes, 0, (int)needed).Split('\0')) {
                        if (String.Equals(id, HardwareId, StringComparison.OrdinalIgnoreCase)) return true;
                    }
                }
            }
            if (Marshal.GetLastWin32Error() != 259) throw new Win32Exception(Marshal.GetLastWin32Error());
            return false;
        } finally { SetupDiDestroyDeviceInfoList(set); }
    }
    static bool NeedsRestart(IntPtr set, ref DeviceInfo data) {
        var parameters = new InstallParams { Size=(uint)Marshal.SizeOf(typeof(InstallParams)), DriverPath=String.Empty };
        Check(SetupDiGetDeviceInstallParamsW(set, ref data, ref parameters));
        return (parameters.Flags & 0x180) != 0; // DI_NEEDRESTART | DI_NEEDREBOOT
    }
    public static bool CreateRoot() {
        // A second installer may have raced staging. Never update its root.
        if (HasRootDevice()) throw new InvalidOperationException("A shared USB/IP controller appeared during installation; leave it unchanged and retry.");
        Guid guid = UsbClass;
        IntPtr set = SetupDiCreateDeviceInfoList(ref guid, IntPtr.Zero);
        CheckSet(set);
        bool registered=false, installed=false;
        var data = new DeviceInfo { Size=(uint)Marshal.SizeOf(typeof(DeviceInfo)) };
        try {
            Check(SetupDiCreateDeviceInfoW(set, "USB", ref guid, null, IntPtr.Zero, 1, ref data));
            byte[] ids = Encoding.Unicode.GetBytes(HardwareId + "\0\0");
            Check(SetupDiSetDeviceRegistryPropertyW(set, ref data, 1, ids, (uint)ids.Length));
            Check(SetupDiCallClassInstaller(0x19, set, ref data)); // DIF_REGISTERDEVICE
            registered=true;
            bool registrationReboot = NeedsRestart(set, ref data);
            bool reboot;
            // Target only this exact new root. The staged driver may already
            // have auto-bound; a hardware-ID-wide UpdateDriver would report
            // ERROR_NO_MORE_ITEMS or touch a shared controller racing us.
            Check(DiInstallDevice(IntPtr.Zero, set, ref data, IntPtr.Zero, 0, out reboot));
            byte[] serviceBytes = new byte[512]; uint type, needed;
            Check(SetupDiGetDeviceRegistryPropertyW(set, ref data, 4, out type, serviceBytes, (uint)serviceBytes.Length, out needed));
            if (!String.Equals(Encoding.Unicode.GetString(serviceBytes, 0, (int)needed).TrimEnd('\0'), "usbip2_ude", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("The new USB/IP root did not bind to its signed controller driver.");
            installed=true;
            return reboot || registrationReboot || NeedsRestart(set, ref data);
        } finally {
            // Roll back only the exact root this call created if binding fails.
            if (registered && !installed) SetupDiCallClassInstaller(5, set, ref data); // DIF_REMOVE
            SetupDiDestroyDeviceInfoList(set);
        }
    }
}
'@
    $mutex = New-Object Threading.Mutex($false, 'Global\VibeshineUsbIpInstall')
    $locked = $false
    try {
        try { $locked = $mutex.WaitOne(60000) } catch [Threading.AbandonedMutexException] { $locked = $true }
        if (!$locked) { throw 'Another Vibepollo USB/IP installation is still running.' }

        $hasDeviceOrService = (Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\usbip2_ude') -or
                  (Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\usbip2_filter') -or
                  [VibepolloUsbIpSetup]::HasRootDevice()
        $stateKey = 'HKLM:\SOFTWARE\Vibepollo\UsbIpInstall'
        $state = Get-ItemProperty -LiteralPath $stateKey -ErrorAction SilentlyContinue
        $ownIncompleteStaging = $state -and $state.Incomplete -eq 1 -and
                                $state.InstallerSha256 -eq $UsbIpInstallerHash
        $lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'release-lock.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        $stagedPackages = @()
        # Detect staged packages too, without relying on localized pnputil text.
        if (!$hasDeviceOrService) {
            foreach ($inf in Get-ChildItem -LiteralPath (Join-Path $env:windir 'INF') -Filter 'oem*.inf') {
                $match = Select-String -LiteralPath $inf.FullName -Pattern '^\s*CatalogFile\s*=\s*usbip2_(ude|filter)\.cat\s*$'
                if ($match) {
                    $stagedPackages += $inf.FullName
                    $name = 'usbip2_' + $match[0].Matches[0].Groups[1].Value + '.inf'
                    if ((Get-FileHash -LiteralPath $inf.FullName -Algorithm SHA256).Hash -ne $lock.files.$name) {
                        $ownIncompleteStaging = $false
                    }
                }
            }
        }
        if ($hasDeviceOrService -or ($stagedPackages.Count -gt 0 -and !$ownIncompleteStaging)) {
            if ([VibepolloUsbIpSetup]::Available()) {
                Write-Output '[VibepolloUsbIp] Reusing the existing shared USB/IP transport without changing it.'
                if ($state) { Remove-Item -LiteralPath $stateKey -Force }
            } else {
                Write-Output '[VibepolloUsbIp] USBIP_TRANSPORT_INFO: Shared USB/IP drivers already exist but the controller is unavailable. Vibepollo leaves them unchanged. Optional DualSense USB waveform haptics may be unavailable until Windows is restarted or that installation is repaired.'
            }
            exit 0
        }

        # Keep retries possible after our own interrupted staging. This marker
        # never authorizes changing a root/service or a different package: only
        # byte-identical store-only leftovers from this pinned attempt qualify.
        New-Item -Path $stateKey -Force | Out-Null
        New-ItemProperty -LiteralPath $stateKey -Name Incomplete -Value 1 -PropertyType DWord -Force | Out-Null
        New-ItemProperty -LiteralPath $stateKey -Name InstallerSha256 -Value $UsbIpInstallerHash -PropertyType String -Force | Out-Null

        $pnputil = Join-Path $env:windir 'System32\pnputil.exe'
        $rebootRequired = $false
        foreach ($name in @('usbip2_filter.inf', 'usbip2_ude.inf')) {
            # Stage without /install: the filter is selected for the new virtual
            # hub during enumeration; do not force a restart of physical hubs.
            & $pnputil /add-driver (Join-Path $PSScriptRoot $name)
            if ($LASTEXITCODE -eq 3010) { $rebootRequired = $true }
            elseif ($LASTEXITCODE -ne 0) { throw "USB/IP Driver Store staging failed for $name (exit $LASTEXITCODE)." }
        }
        if ([VibepolloUsbIpSetup]::CreateRoot()) { $rebootRequired = $true }
        for ($i=0; $i -lt 40 -and ![VibepolloUsbIpSetup]::Available(); ++$i) { Start-Sleep -Milliseconds 250 }
        if ($rebootRequired) { Write-Output '[VibepolloUsbIp] USBIP_TRANSPORT_RESTART_REQUIRED' }
        if (![VibepolloUsbIpSetup]::Available()) {
            Write-Output '[VibepolloUsbIp] USBIP_TRANSPORT_INFO: Driver installation completed but the controller is not ready. Restart Windows before using optional DualSense USB waveform haptics.'
            Write-Output '[VibepolloUsbIp] USBIP_TRANSPORT_RESTART_REQUIRED'
        } else {
            Remove-Item -LiteralPath $stateKey -Force
            Write-Output '[VibepolloUsbIp] Signed USB/IP transport is ready for DualSense USB waveform haptics with a compatible Moonlight client.'
        }
    } finally {
        if ($locked) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
} catch {
    Write-UsbIpWarning $_.Exception.Message
    if ($InstallerBestEffort) { exit 0 }
    exit 1
}
