# One in-memory snapshot for the installer process, shared by its driver runspaces.
# No disk cache or caller-supplied inventory can become an elevated trust input.
if (-not ('VibeshineDriverInventory' -as [type])) {
    $inventorySource = @'
using System;
using System.IO;
using System.Text;
using System.ComponentModel;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32;
public static class VibeshineDriverInventory {
    public sealed class Package {
        public string OriginalFileName, OriginalName, Driver, PublishedName;
    }
    public sealed class Device {
        public string DeviceID, DriverVersion, HardwareID;
        public bool Present;
        public uint ConfigManagerErrorCode;
    }
    public sealed class Snapshot {
        public Package[] Packages;
        public Device[] Devices;
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct OriginalInfo {
        public uint Size;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=260)] public string Inf;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=260)] public string Catalog;
    }
    [StructLayout(LayoutKind.Sequential)] struct DeviceInfo {
        public uint Size; public Guid Class; public uint DevInst; public UIntPtr Reserved;
    }
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool SetupGetInfInformation(string path, uint search, IntPtr buffer, uint size, out uint required);
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool SetupQueryInfOriginalFileInformation(IntPtr info, uint index, IntPtr platform, ref OriginalInfo original);
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool SetupGetInfDriverStoreLocation(string inf, IntPtr platform, string locale, StringBuilder store, uint size, out uint required);
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr SetupDiGetClassDevs(IntPtr guid, string enumerator, IntPtr window, uint flags);
    [DllImport("setupapi.dll", SetLastError=true)]
    static extern bool SetupDiEnumDeviceInfo(IntPtr set, uint index, ref DeviceInfo info);
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool SetupDiGetDeviceRegistryProperty(IntPtr set, ref DeviceInfo info, uint property, out uint type, byte[] buffer, uint size, out uint required);
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool SetupDiGetDeviceInstanceId(IntPtr set, ref DeviceInfo info, StringBuilder id, uint size, out uint required);
    [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("cfgmgr32.dll")] static extern uint CM_Get_DevNode_Status(out uint status, out uint problem, uint instance, uint flags);
    static readonly object Gate = new object();
    static Snapshot cached;
    public static int PackageQueries, DeviceQueries, DeviceSets, Hits, Invalidations;
    public static long PackageMilliseconds, DeviceMilliseconds;
    static readonly string[] Infs = { "SunshineVirtualDisplayDriver.inf", "SudoVDA.inf", "VibeshineVhfGamepad.inf", "VibeshineVhfMouseMapper.inf" };
    static bool OwnedInf(string name) { foreach (string inf in Infs) if (String.Equals(name,inf,StringComparison.OrdinalIgnoreCase)) return true; return false; }
    static string[] Property(IntPtr set, ref DeviceInfo info, uint property) {
        uint type, required;
        if (!SetupDiGetDeviceRegistryProperty(set,ref info,property,out type,null,0,out required)) {
            int error=Marshal.GetLastWin32Error();
            if (error==13) return new string[0]; // property absent, not an enumeration failure
            if (error!=122) throw new Win32Exception(error,"Device inventory property failed");
        }
        if (required==0) return new string[0];
        byte[] bytes=new byte[required];
        if (!SetupDiGetDeviceRegistryProperty(set,ref info,property,out type,bytes,required,out required)) throw new Win32Exception(Marshal.GetLastWin32Error());
        return Encoding.Unicode.GetString(bytes).TrimEnd('\0').Split('\0');
    }
    static Package[] ReadPackages() {
        var clock=Stopwatch.StartNew(); PackageQueries++;
        var result=new List<Package>();
        string root=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows),"INF");
        // SetupAPI supplies original OEM identities without DISM's inbox scan.
        foreach (string path in Directory.GetFiles(root,"oem*.inf")) {
            uint size;
            if (!SetupGetInfInformation(path,2,IntPtr.Zero,0,out size)) throw new Win32Exception(Marshal.GetLastWin32Error(),"Cannot inventory published INF");
            IntPtr buffer=Marshal.AllocHGlobal((int)size);
            try {
                if (!SetupGetInfInformation(path,2,buffer,size,out size)) throw new Win32Exception(Marshal.GetLastWin32Error());
                var original=new OriginalInfo { Size=(uint)Marshal.SizeOf(typeof(OriginalInfo)) };
                if (!SetupQueryInfOriginalFileInformation(buffer,0,IntPtr.Zero,ref original)) throw new Win32Exception(Marshal.GetLastWin32Error());
                if (OwnedInf(original.Inf)) {
                    var store=new StringBuilder(32768); uint required;
                    if (!SetupGetInfDriverStoreLocation(path,IntPtr.Zero,null,store,(uint)store.Capacity,out required)) throw new Win32Exception(Marshal.GetLastWin32Error());
                    if (!String.Equals(Path.GetFileName(store.ToString()),original.Inf,StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("Published and stored INF identities disagree");
                    result.Add(new Package { OriginalFileName=store.ToString(), OriginalName=original.Inf, Driver=Path.GetFileName(path), PublishedName=Path.GetFileName(path) });
                }
            } finally { Marshal.FreeHGlobal(buffer); }
        }
        PackageMilliseconds+=clock.ElapsedMilliseconds;
        Console.WriteLine("DRIVER_QUERY:packages count="+PackageQueries+" elapsed_ms="+clock.ElapsedMilliseconds);
        return result.ToArray();
    }
    static Device[] ReadDevices() {
        var clock=Stopwatch.StartNew(); DeviceQueries++;
        var result=new List<Device>();
        // Our source/display nodes use ROOT; the mapper binds a HID child.
        // Do not enumerate PCI, USB, software devices or unrelated providers.
        foreach(string enumerator in new string[] { "ROOT", "HID" }) {
        DeviceSets++;
        IntPtr set=SetupDiGetClassDevs(IntPtr.Zero,enumerator,IntPtr.Zero,4); // ALLCLASSES in this enumerator; includes disconnected nodes
        if (set==new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            for (uint index=0;;index++) {
                var info=new DeviceInfo { Size=(uint)Marshal.SizeOf(typeof(DeviceInfo)) };
                if (!SetupDiEnumDeviceInfo(set,index,ref info)) {
                    int error=Marshal.GetLastWin32Error(); if (error==259) break; throw new Win32Exception(error);
                }
                string[] hardware=Property(set,ref info,1);
                string[] compatible=Property(set,ref info,2);
                string matchingId=null;
                foreach (string id in Combine(hardware,compatible)) {
                    if (id.Equals(@"ROOT\SunshineVirtualDisplay",StringComparison.OrdinalIgnoreCase) ||
                              id.Equals(@"ROOT\SUDOMAKER\SUDOVDA",StringComparison.OrdinalIgnoreCase) ||
                              id.Equals(@"ROOT\VIBESHINEVIRTUALGAMEPAD",StringComparison.OrdinalIgnoreCase) ||
                              (id.StartsWith(@"HID\VID_",StringComparison.OrdinalIgnoreCase) && id.EndsWith("&REV_0100&Col02",StringComparison.OrdinalIgnoreCase))) { matchingId=id; break; }
                }
                if(matchingId==null) continue;
                // Query software keys only for the exact hardware we service.
                // Keep unbound owned nodes visible as unknown, never "missing".
                string version="";
                string[] keys=Property(set,ref info,9); // SPDRP_DRIVER
                if(keys.Length!=0) using (RegistryKey key=Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\"+keys[0])) {
                    if (key==null) throw new InvalidOperationException("Cannot read owned device driver identity");
                    version=key.GetValue("DriverVersion","") as string;
                }
                var instance=new StringBuilder(1024); uint length;
                if (!SetupDiGetDeviceInstanceId(set,ref info,instance,(uint)instance.Capacity,out length)) throw new Win32Exception(Marshal.GetLastWin32Error());
                uint status, problem; uint cr=CM_Get_DevNode_Status(out status,out problem,info.DevInst,0);
                if (cr!=0 && cr!=13) throw new InvalidOperationException("Cannot query owned device status: "+cr);
                result.Add(new Device { DeviceID=instance.ToString(), HardwareID=matchingId,
                    DriverVersion=version, Present=cr==0, ConfigManagerErrorCode=cr==0 ? problem : 45 });
            }
        } finally { SetupDiDestroyDeviceInfoList(set); }
        }
        DeviceMilliseconds+=clock.ElapsedMilliseconds;
        Console.WriteLine("DRIVER_QUERY:devices count="+DeviceQueries+" elapsed_ms="+clock.ElapsedMilliseconds);
        return result.ToArray();
    }
    static IEnumerable<string> Combine(string[] first,string[] second) { foreach(string v in first) yield return v; foreach(string v in second) yield return v; }
    public static Snapshot Get() {
        lock(Gate) {
            if(cached!=null) { Hits++; return cached; }
            // Publish only a complete snapshot. Errors never become a missing-driver result.
            var query=System.Threading.Tasks.Task.Factory.StartNew(delegate {
                return new Snapshot { Packages=ReadPackages(), Devices=ReadDevices() };
            });
            if(!query.Wait(15000)) throw new TimeoutException("Native driver inventory exceeded 15 seconds; incomplete results discarded. Driver unchanged.");
            cached=query.Result; return cached;
        }
    }
    public static void Invalidate() { lock(Gate) { cached=null; Invalidations++; } }
    public static string Summary() { return "DRIVER_QUERY_SUMMARY:packages="+PackageQueries+" devices="+DeviceQueries+" device_sets="+DeviceSets+" hits="+Hits+" invalidations="+Invalidations+" packages_ms="+PackageMilliseconds+" devices_ms="+DeviceMilliseconds; }
}
'@
    # Use the existing large-environment-safe compiler when supplied by a driver.
    if (Get-Command Add-DriverInteropType -ErrorAction SilentlyContinue) {
        Add-DriverInteropType -TypeDefinition $inventorySource
    } elseif ($PSVersionTable.PSEdition -eq 'Core') {
        Add-Type -TypeDefinition $inventorySource
    } else {
        $runtime = [Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()
        $temporary = Join-Path ([IO.Path]::GetTempPath()) ('inventory-interop-' + [guid]::NewGuid().ToString('N'))
        $null = New-Item -ItemType Directory -Path $temporary
        try {
            $source = Join-Path $temporary 'inventory.cs'; $assembly = Join-Path $temporary 'inventory.dll'
            [IO.File]::WriteAllText($source, $inventorySource)
            & (Join-Path $runtime 'csc.exe') /nologo /target:library /out:$assembly $source
            if ($LASTEXITCODE -ne 0) { throw 'Driver inventory compilation failed.' }
            $null = [Reflection.Assembly]::Load([IO.File]::ReadAllBytes($assembly))
        } finally {
            $resolved = [IO.Path]::GetFullPath($temporary)
            $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
            if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^inventory-interop-[0-9a-f]{32}$') { throw 'Unsafe inventory compiler cleanup path' }
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
}

function Get-SharedDriverInventory {
    param([Parameter(Mandatory=$true)][string]$InfName, [Parameter(Mandatory=$true)][string]$HardwareId)
    $snapshot = [VibeshineDriverInventory]::Get()
    # The published OEM name is authoritative; retrieve its original identity
    # once in the native provider and retain that name separately for filtering.
    [pscustomobject]@{
        Packages = @($snapshot.Packages | Where-Object { $_.OriginalName -ieq $InfName })
        Devices = @($snapshot.Devices | Where-Object { $_.HardwareID -ieq $HardwareId -or $_.DeviceID.StartsWith($HardwareId + '\', [StringComparison]::OrdinalIgnoreCase) })
    }
}
