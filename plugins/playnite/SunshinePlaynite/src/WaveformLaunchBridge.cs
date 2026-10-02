using System;
using System.IO;
using System.Diagnostics;
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
            if (steam)
            {
                var helper = Path.Combine(Path.GetDirectoryName(path), "vibepollo_dualsense_haptics.exe");
                var start = new ProcessStartInfo(helper, "--prepare-steam");
                start.UseShellExecute = false;
                start.CreateNoWindow = true;
                start.RedirectStandardError = true;
                start.EnvironmentVariables["VIBEPOLLO_DUALSENSE_HAPTICS_MAPPING"] = mapping;
                start.EnvironmentVariables["VIBEPOLLO_DUALSENSE_HAPTICS_DIRECTORY"] = directory;
                using (var process = Process.Start(start))
                {
                    if (!process.WaitForExit(25000))
                        throw new InvalidOperationException("Steam waveform preparation timed out.");
                    var error = process.StandardError.ReadToEnd().Trim();
                    if (process.ExitCode != 0)
                        throw new InvalidOperationException(string.IsNullOrEmpty(error) ? "Steam waveform preparation failed." : error);
                    return true;
                }
            }
            if (prepare == null)
            {
                var library = LoadLibraryExW(path, IntPtr.Zero, 8);
                if (library == IntPtr.Zero)
                    throw new InvalidOperationException("Could not load waveform audio hook (Windows error " + Marshal.GetLastWin32Error() + ").");
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
