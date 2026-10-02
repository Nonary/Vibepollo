using System;
using System.IO;
using System.Runtime.InteropServices;
namespace SunshinePlaynite
{
public static class WaveformLaunchBridge
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);
    [UnmanagedFunctionPointer(CallingConvention.StdCall, CharSet = CharSet.Unicode)]
    private delegate uint PrepareLaunch([MarshalAs(UnmanagedType.LPWStr)] string mapping,
        [MarshalAs(UnmanagedType.LPWStr)] string directory, [MarshalAs(UnmanagedType.Bool)] bool steam);
    private static readonly object Gate = new object();
    private static string loadedPath;
    private static PrepareLaunch prepare;
    public static bool Prepare(string path, string mapping, string directory, bool steam)
    {
        if (string.IsNullOrWhiteSpace(path) || !Path.IsPathRooted(path) ||
            string.IsNullOrWhiteSpace(directory) || !Directory.Exists(directory)) return false;
        lock (Gate)
        {
            path = Path.GetFullPath(path);
            if (prepare == null)
            {
                var library = LoadLibraryExW(path, IntPtr.Zero, 8);
                if (library == IntPtr.Zero) return false;
                var entry = GetProcAddress(library, "VibepolloHapticsPreparePlaynite");
                if (entry == IntPtr.Zero) return false;
                prepare = (PrepareLaunch)Marshal.GetDelegateForFunctionPointer(entry, typeof(PrepareLaunch));
                loadedPath = path;
            }
            // A running Playnite keeps its native hook loaded. Changing host
            // installations requires restarting it rather than mixing DLLs.
            if (!string.Equals(path, loadedPath, StringComparison.OrdinalIgnoreCase)) return false;
            return prepare(mapping, directory, steam) != 0;
        }
    }
}
}
