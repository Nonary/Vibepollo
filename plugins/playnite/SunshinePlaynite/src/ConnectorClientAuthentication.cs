using System;
using System.Collections.Generic;
using System.ComponentModel;

namespace SunshinePlaynite
{
    internal static class ConnectorClientAuthentication
    {
        public static string Validate(IDictionary<string, object> hello, Func<uint> getClientPid,
            Func<uint, string> getExecutable, Func<bool> isLocalSystemClient)
        {
            if (!string.Equals(GetString(hello, "type"), "hello", StringComparison.Ordinal))
                throw new UnauthorizedAccessException("Invalid client hello message");
            var role = GetString(hello, "role");
            var launcher = string.Equals(role, "launcher", StringComparison.OrdinalIgnoreCase);
            if (!launcher && !string.Equals(role, "sunshine", StringComparison.OrdinalIgnoreCase))
                throw new UnauthorizedAccessException("Invalid client role");
            var claimedPid = GetInt(hello, "pid");
            if (!claimedPid.HasValue || claimedPid.Value <= 0 || getClientPid() != (uint)claimedPid.Value)
                throw new UnauthorizedAccessException("Client PID validation failed");

            string executable;
            try
            {
                executable = getExecutable((uint)claimedPid.Value);
            }
            catch (Win32Exception ex) when (ex.NativeErrorCode == 5 && !launcher)
            {
                // Normal-user Playnite cannot query a SYSTEM host. LocalSystem is
                // already trusted by the pipe ACL and can control the host itself.
                // Only a token from this connection can authorize this fallback;
                // access denial, a claimed PID/name, or an administrator SID cannot.
                if (!isLocalSystemClient())
                    throw new UnauthorizedAccessException("Inaccessible pipe client is not LocalSystem", ex);
                return role;
            }
            var expected = launcher ? "playnite-launcher.exe" : "sunshine.exe";
            if (!string.Equals(executable, expected, StringComparison.OrdinalIgnoreCase))
                throw new UnauthorizedAccessException("Client executable does not match its declared role");
            return role;
        }

        private static string GetString(IDictionary<string, object> value, string key)
        {
            object item;
            return value != null && value.TryGetValue(key, out item) && item != null
                ? Convert.ToString(item) : string.Empty;
        }

        private static int? GetInt(IDictionary<string, object> value, string key)
        {
            object item;
            if (value == null || !value.TryGetValue(key, out item) || item == null) return null;
            try { return Convert.ToInt32(item); } catch { return null; }
        }
    }
}
