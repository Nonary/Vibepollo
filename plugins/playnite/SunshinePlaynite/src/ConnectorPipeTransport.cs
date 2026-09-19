using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using System.Threading;

namespace SunshinePlaynite
{
    internal static class ConnectorPipeTransport
    {
        private const uint ProcessQueryLimitedInformation = 0x1000;

        public static NamedPipeServerStream CreateServer(string name, PipeSecurity security)
        {
            const PipeOptions options = PipeOptions.Asynchronous;
            if (security == null) throw new ArgumentNullException(nameof(security));
            return new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
                options, 65536, 65536, security);
        }

        public static PipeSecurity CreatePipeSecurity()
        {
            SecurityIdentifier user;
            using (var identity = WindowsIdentity.GetCurrent())
            {
                user = identity.User;
            }
            if (user == null) throw new InvalidOperationException("Could not resolve the Playnite user SID");

            var security = new PipeSecurity();
            security.SetAccessRuleProtection(true, false);
            security.SetOwner(user);
            security.AddAccessRule(new PipeAccessRule(
                new SecurityIdentifier(WellKnownSidType.NetworkSid, null),
                PipeAccessRights.ReadWrite,
                AccessControlType.Deny));
            security.AddAccessRule(new PipeAccessRule(
                new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
                PipeAccessRights.FullControl,
                AccessControlType.Allow));
            security.AddAccessRule(new PipeAccessRule(user, PipeAccessRights.FullControl, AccessControlType.Allow));
            return security;
        }

        public static void WaitForConnection(NamedPipeServerStream pipe, CancellationToken token, int timeoutMs = Timeout.Infinite)
        {
            var wait = pipe.WaitForConnectionAsync();
            var elapsed = Stopwatch.StartNew();
            while (!wait.Wait(200))
            {
                token.ThrowIfCancellationRequested();
                if (timeoutMs != Timeout.Infinite && elapsed.ElapsedMilliseconds >= timeoutMs)
                    throw new TimeoutException("Client did not connect to the data pipe");
            }
            if (!pipe.IsConnected) throw new IOException("Pipe failed to connect");
        }

        public static string ReadLine(StreamReader reader, CancellationToken token, int timeoutMs)
        {
            var read = reader.ReadLineAsync();
            var elapsed = Stopwatch.StartNew();
            while (!read.Wait(200))
            {
                token.ThrowIfCancellationRequested();
                if (elapsed.ElapsedMilliseconds >= timeoutMs)
                    throw new TimeoutException("Client did not send a hello message");
            }
            return read.GetAwaiter().GetResult();
        }

        public static string ValidateClient(NamedPipeServerStream pipe, IDictionary<string, object> hello)
        {
            if (!string.Equals(GetString(hello, "type"), "hello", StringComparison.Ordinal))
                throw new UnauthorizedAccessException("Invalid client hello message");

            var role = GetString(hello, "role");
            if (!string.Equals(role, "sunshine", StringComparison.OrdinalIgnoreCase) &&
                !string.Equals(role, "launcher", StringComparison.OrdinalIgnoreCase))
                throw new UnauthorizedAccessException("Invalid client role");

            uint actualPid;
            var claimedPid = GetInt(hello, "pid");
            if (!claimedPid.HasValue || claimedPid.Value <= 0 ||
                !GetNamedPipeClientProcessId(pipe.SafePipeHandle, out actualPid) ||
                actualPid != (uint)claimedPid.Value)
                throw new UnauthorizedAccessException("Client PID validation failed");

            var executable = GetProcessExecutableName(actualPid);
            var validExecutable = string.Equals(role, "launcher", StringComparison.OrdinalIgnoreCase)
                ? string.Equals(executable, "playnite-launcher.exe", StringComparison.OrdinalIgnoreCase)
                : string.Equals(executable, "sunshine.exe", StringComparison.OrdinalIgnoreCase) ||
                  string.Equals(executable, "vibeshine.exe", StringComparison.OrdinalIgnoreCase);
            if (!validExecutable)
                throw new UnauthorizedAccessException("Client executable does not match its declared role");

            return role;
        }

        public static void WriteHandshake(Stream control, string pipeName)
        {
            var chars = new char[40];
            var source = (pipeName.ToUpperInvariant() + '\0').ToCharArray();
            Array.Copy(source, chars, Math.Min(source.Length, chars.Length));
            var bytes = Encoding.Unicode.GetBytes(chars);
            control.Write(bytes, 0, bytes.Length);
            control.Flush();
        }

        public static bool WaitForAck(Stream control)
        {
            var buffer = new byte[1];
            var read = control.ReadAsync(buffer, 0, 1);
            return read.Wait(1500) && read.Result == 1 && buffer[0] == 0x02;
        }

        private static string GetProcessExecutableName(uint processId)
        {
            var process = OpenProcess(ProcessQueryLimitedInformation, false, processId);
            if (process == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not inspect the pipe client process");
            try
            {
                var capacity = 32768;
                var path = new StringBuilder(capacity);
                if (!QueryFullProcessImageName(process, 0, path, ref capacity))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not resolve the pipe client executable");
                return Path.GetFileName(path.ToString());
            }
            finally
            {
                CloseHandle(process);
            }
        }

        private static string GetString(IDictionary<string, object> value, string key)
        {
            object item;
            return value != null && value.TryGetValue(key, out item) && item != null
                ? Convert.ToString(item)
                : string.Empty;
        }

        private static int? GetInt(IDictionary<string, object> value, string key)
        {
            object item;
            if (value == null || !value.TryGetValue(key, out item) || item == null) return null;
            try { return Convert.ToInt32(item); } catch { return null; }
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetNamedPipeClientProcessId(
            Microsoft.Win32.SafeHandles.SafePipeHandle pipe,
            out uint clientProcessId);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr OpenProcess(uint desiredAccess, bool inheritHandle, uint processId);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool QueryFullProcessImageName(
            IntPtr process,
            int flags,
            StringBuilder executablePath,
            ref int size);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr handle);
    }
}
