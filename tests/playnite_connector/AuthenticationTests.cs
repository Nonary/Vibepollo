using System;
using System.Collections.Generic;
using System.ComponentModel;
using SunshinePlaynite;

internal static class AuthenticationTests
{
    private static Dictionary<string, object> Hello(string role = "sunshine", object pid = null) =>
        new Dictionary<string, object> { ["type"] = "hello", ["role"] = role, ["pid"] = pid ?? 42 };

    private static void Expect<T>(Action action, string message) where T : Exception
    {
        try { action(); }
        catch (T) { Console.WriteLine("PASS: " + message); return; }
        throw new Exception(message);
    }

    public static void Run()
    {
        var tokenChecks = 0;
        Func<bool> system = () => { tokenChecks++; return true; };
        Func<uint, string> denied = pid => throw new Win32Exception(5);
        var role = ConnectorClientAuthentication.Validate(Hello(), () => 42, denied, system);
        if (role != "sunshine" || tokenChecks != 1) throw new Exception("SYSTEM service acceptance");
        Console.WriteLine("PASS: inaccessible service requires connected LocalSystem identity");

        Expect<UnauthorizedAccessException>(() => ConnectorClientAuthentication.Validate(
            Hello(), () => 42, denied, () => false), "Access denied without LocalSystem is rejected");
        Expect<Win32Exception>(() => ConnectorClientAuthentication.Validate(
            Hello("launcher"), () => 42, denied, system), "Launcher cannot use SYSTEM fallback");
        Expect<Win32Exception>(() => ConnectorClientAuthentication.Validate(
            Hello(), () => 42, pid => throw new Win32Exception(87), system),
            "Non-access-denied inspection errors remain rejected");
        Expect<InvalidOperationException>(() => ConnectorClientAuthentication.Validate(
            Hello(), () => 42, denied, () => throw new InvalidOperationException("No token")),
            "Failed pipe token inspection remains rejected");
        Expect<UnauthorizedAccessException>(() => ConnectorClientAuthentication.Validate(
            Hello(), () => 43, denied, system), "Spoofed PID cannot reach token fallback");
        Expect<UnauthorizedAccessException>(() => ConnectorClientAuthentication.Validate(
            Hello("other"), () => 42, denied, system), "Unknown role cannot reach token fallback");
        foreach (var pid in new object[] { 0, -1, "invalid", long.MaxValue })
            Expect<UnauthorizedAccessException>(() => ConnectorClientAuthentication.Validate(
                Hello(pid: pid), () => 42, denied, system), "Invalid PID is rejected");
        var malformed = Hello();
        malformed["type"] = "command";
        Expect<UnauthorizedAccessException>(() => ConnectorClientAuthentication.Validate(
            malformed, () => 42, denied, system), "Commands cannot replace hello authentication");
        Expect<UnauthorizedAccessException>(() => ConnectorClientAuthentication.Validate(
            null, () => 42, denied, system), "Missing hello is rejected");
        Expect<UnauthorizedAccessException>(() => ConnectorClientAuthentication.Validate(
            Hello(), () => 42, pid => "other.exe", system), "Known wrong executable never uses SYSTEM fallback");
        Expect<UnauthorizedAccessException>(() => ConnectorClientAuthentication.Validate(
            Hello("launcher"), () => 42, pid => "sunshine.exe", system), "Executable role mismatch is rejected");
        foreach (var allowed in new[] { "sunshine", "launcher" })
        {
            var result = ConnectorClientAuthentication.Validate(Hello(allowed), () => 42,
                pid => allowed == "launcher" ? "Playnite-Launcher.EXE" : "Sunshine.EXE", system);
            if (result != allowed) throw new Exception("Normal executable authentication");
            Console.WriteLine("PASS: normal " + allowed + " executable authentication");
        }
        if (tokenChecks != 1) throw new Exception("Token fallback used outside permitted case");
    }
}
