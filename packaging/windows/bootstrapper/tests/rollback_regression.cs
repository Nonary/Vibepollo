using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using PRODUCT_NAMESPACE;

internal static class RollbackRegression {
  internal const string Code = "{11111111-1111-1111-1111-111111111111}";
  private const string Other = "{22222222-2222-2222-2222-222222222222}";
  internal static string Root, CachePath, IncomingPath, LastRecoveryDirectory;
  internal static InstallerRunner.InstalledProductInfo Installed, RestoredProduct;
  internal static InstallerRunner.InstalledProductInfo InstalledTarget, RestoredTarget;
  internal static InstallerRunner.PayloadMsiInfo Incoming, Cached;
  internal static readonly List<string> Events = new List<string>();
  internal static List<string> RestoreArguments;
  internal static bool MetadataThrows, DirectoryFails, CopyFails, RestoreThrows, Hidden, Elevate;
  internal static int UninstallCode, RestoreCode, Checks;
  internal static bool NativeRegistrationPresent, NativeStateThrows, RestoreMakesRegistration;
  internal static int NativeProductState, RegistrationReadFailure;
  internal static void Check(bool condition, string name) {
    if (!condition) throw new Exception(name);
    Checks++;
  }
  private static object Call(string name, params object[] args) {
    try {
      return typeof(InstallerRunner).GetMethod(name, BindingFlags.Static | BindingFlags.NonPublic).Invoke(null, args);
    } catch (TargetInvocationException ex) { throw ex.InnerException; }
  }
  private static InstallerRunner.InstalledProductInfo Product(string code = Code) {
    return new InstallerRunner.InstalledProductInfo { ProductCode = code, DisplayName = "Vibeshine",
      Kind = InstallerRunner.InstalledProductKind.Vibeshine, IsWindowsInstaller = true,
      InstallLocation = Path.Combine(Root, "installed root ' & 日本"), Version = new Version(2, 0, 99, 0) };
  }
  private static void Reset(bool transactional = false, string payloadCode = Other) {
    Events.Clear(); RestoreArguments = null; LastRecoveryDirectory = null;
    RegistrationReadFailure = 0;
    MetadataThrows = DirectoryFails = CopyFails = RestoreThrows = false;
    Hidden = true; Elevate = false; UninstallCode = RestoreCode = 0; NativeRegistrationPresent = true; NativeProductState = -1; NativeStateThrows = RestoreMakesRegistration = false;
    CachePath = Path.Combine(Root, "cached.msi");
    File.WriteAllBytes(CachePath, new byte[] { 0, 1, 2, 127, 255 });
    IncomingPath = Path.Combine(Root, "incoming.msi");
    Installed = Product(); RestoredProduct = Product();
    InstalledTarget = RestoredTarget = null;
    Incoming = new InstallerRunner.PayloadMsiInfo { ProductCode = payloadCode, Version = Installed.Version,
      SupportsTransactionalReplacement = transactional };
    Cached = new InstallerRunner.PayloadMsiInfo { ProductCode = Code.ToLowerInvariant() };
  }
  private static InstallerResult PreUninstall(out InstallerRunner.StashedVibeshinePayload stash, string phase = "install_remove_source") {
    var args = new object[] { IncomingPath, phase, Hidden, Elevate, null };
    var result = (InstallerResult)Call("TryPreUninstallDowngradeSourceVersion", args);
    stash = (InstallerRunner.StashedVibeshinePayload)args[4];
    return result;
  }
  private static void CheckBlocked(string reason) {
    var prior = Installed;
    InstallerRunner.StashedVibeshinePayload stash;
    var result = PreUninstall(out stash);
    Check(result != null && result.ExitCode == 1603 && !result.Succeeded, reason + ": fail closed");
    Check(stash == null && !Events.Contains("uninstall") && object.ReferenceEquals(prior, Installed), reason + ": no pre-uninstall or lost installation");
    Check(result.Message.Contains("exact rollback copy") && result.ProductCode == Code, reason + ": preservation failure identifies product");
    Check(LastRecoveryDirectory == null || !Directory.Exists(LastRecoveryDirectory), reason + ": failed stash cleaned");
  }
  private static InstallerRunner.StashedVibeshinePayload RemoveLegacy() {
    InstallerRunner.StashedVibeshinePayload stash;
    var result = PreUninstall(out stash);
    Check(result != null && result.Succeeded && stash != null && Installed == null, "legacy removal returns preserved rollback payload");
    Check(stash.ProductCode == Code && stash.InstallLocation == Product().InstallLocation && File.Exists(stash.MsiPath), "stash records exact product and custom root");
    return stash;
  }
  private static InstallerResult Recover(InstallerRunner.StashedVibeshinePayload stash, int code = 1603) {
    var failure = new InstallerResult { Operation = InstallerOperation.Install, ExitCode = code, Message = "Incoming install failed.", LogPath = "original.log" };
    var result = (InstallerResult)Call("ApplyStashedPayloadRecovery", failure, stash, "restore_previous");
    Check(object.ReferenceEquals(result, failure) && result.ExitCode == code && result.LogPath == "original.log", "recovery preserves incoming outcome and original log");
    return result;
  }
  public static void Main() {
    Root = Path.Combine(Path.GetTempPath(), "vibeshine-rollback-" + Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(Root);
    try {
      // Equal, newer, and older installed versions all bypass standalone
      // pre-uninstall when the incoming MSI owns transactional replacement.
      foreach (var phase in new[] { "install_remove_source", "cli_remove_source" }) {
        foreach (var transactional in new[] { true, false }) {
          foreach (var version in new[] { new Version(1, 0), new Version(2, 0, 99, 0), new Version(9, 0) }) {
            foreach (var payloadCode in new[] { Code, Other }) {
              Reset(transactional, payloadCode); Installed.Version = version;
              var expected = !transactional && (version > Incoming.Version || (version == Incoming.Version && payloadCode != Code));
              var prior = Installed;
              InstallerRunner.StashedVibeshinePayload stash;
              var result = PreUninstall(out stash, phase);
              Check((result != null) == expected && (stash != null) == expected, "production dispatch follows transactional and legacy version/identity contract");
              if (!expected) Check(Events.Count == 0 && object.ReferenceEquals(prior, Installed), "transactional/upgrade/repair path never touches cache or uninstaller");
              else Check(Events.IndexOf("validate-stash") < Events.IndexOf("uninstall"), "validated stash precedes uninstall");
            }
          }
        }
      }
      Reset(); Installed = null;
      InstallerRunner.StashedVibeshinePayload ignored;
      Check(PreUninstall(out ignored) == null && Events.Count == 0, "fresh install requires no stash");
      Reset(); Installed.Kind = InstallerRunner.InstalledProductKind.Sunshine;
      Check(PreUninstall(out ignored) == null && Events.Count == 0, "competitor remains outside own-product workaround");
      Reset(); Installed.Version = null;
      Check(PreUninstall(out ignored) == null && Events.Count == 0, "unknown source version does not authorize uninstall");
      Reset(); Incoming = null;
      Check(PreUninstall(out ignored) == null && Events.Count == 0, "unreadable payload does not authorize uninstall");
      Reset(); Incoming.Version = null;
      Check(PreUninstall(out ignored) == null && Events.Count == 0, "unknown payload version does not authorize uninstall");
      Reset(); CachePath = null; CheckBlocked("cache unregistered");
      Reset(); File.Delete(CachePath); CheckBlocked("cache missing");
      Reset(); DirectoryFails = true; CheckBlocked("secure directory creation failed");
      Reset(); CopyFails = true; CheckBlocked("cache copy failed");
      Reset(); Cached = null; CheckBlocked("stash MSI unreadable");
      Reset(); Cached.ProductCode = Other; CheckBlocked("stash identity mismatch");
      Reset(); MetadataThrows = true; CheckBlocked("stash metadata query threw");
      foreach (var code in new[] { 0, 3010, 1603 }) {
        foreach (var hidden in new[] { false, true }) {
          foreach (var elevate in new[] { false, true }) {
            Reset(); UninstallCode = code; Hidden = hidden; Elevate = elevate;
            var result = PreUninstall(out ignored);
            Check(result.ExitCode == code && ignored != null && File.Exists(ignored.MsiPath), "retain stash and forward uninstall success/reboot/failure");
          }
        }
      }
      foreach (var code in new[] { 0, 3010 }) {
        Reset(); var stash = RemoveLegacy();
        var result = Recover(stash, code);
        Check(result.Succeeded && !Events.Contains("restore") && !Directory.Exists(stash.RecoveryDirectory), "successful install/reboot cleans stash without restoring");
      }
      foreach (var code in new[] { 0, 3010 }) {
        Reset(); var stash = RemoveLegacy(); RestoreCode = code;
        var result = Recover(stash);
        Check(result.Message.Contains("automatically restored") && Events.Contains("restore"), "failed incoming install attempts production restore");
        Check(RestoreArguments[0] == "/i" && RestoreArguments[1] == stash.MsiPath && RestoreArguments.Contains("/qn") && RestoreArguments.Contains("/norestart"), "restore runs exact stash quietly without reboot");
        Check(RestoreArguments.Contains("SKIP_REMOVE_CONFLICTING_PRODUCTS=1") && RestoreArguments.Contains("REBOOT=ReallySuppress") && RestoreArguments.Contains("SUPPRESSMSGBOXES=1"), "restore preserves existing conflict/reboot suppression");
        Check(RestoreArguments.Contains("INSTALL_ROOT=\"" + stash.InstallLocation + "\""), "restore preserves custom installed root");
        Check(!Directory.Exists(stash.RecoveryDirectory), "verified prior product restoration cleans stash");
      }
      foreach (var mode in new[] { "failure", "unregistered-success", "start-exception", "different-product" }) {
        Reset(); var stash = RemoveLegacy();
        RestoreCode = mode == "failure" ? 1603 : 0;
        RestoredProduct = mode == "different-product" ? Product(Other) : null;
        RestoreThrows = mode == "start-exception";
        var result = Recover(stash);
        Check(Events.Contains("restore") && File.Exists(stash.MsiPath), mode + ": unverified restore retains exact rollback package");
        Check(result.ExitCode == 1603 && result.Message.StartsWith("Incoming install failed."), mode + ": original failure retained");
        if (mode != "different-product") Check(result.Message.Contains("saved to:") && result.Message.Contains(stash.MsiPath), mode + ": retained recovery path reported");
      }
      Reset(); var retained = RemoveLegacy(); Installed = Product(Other);
      Recover(retained);
      Check(!Events.Contains("restore") && File.Exists(retained.MsiPath), "already registered other product is not overwritten and stash retained");
      Reset(); retained = RemoveLegacy(); Installed = Product();
      Recover(retained);
      Check(!Events.Contains("restore") && !Directory.Exists(retained.RecoveryDirectory), "already restored exact product cleans stash without reinstall");
      // Cross-product rollback must verify the exact restored identity in
      // both source and downstream builds, independent of branding getters.
      Reset(); retained = RemoveLegacy(); RestoredProduct = null; RestoredTarget = Product();
      RestoredTarget.Kind = InstallerRunner.InstalledProductKind.Vibepollo;
      var crossProductResult = Recover(retained);
      Check(crossProductResult.Message.Contains("automatically restored"), "restored competitor is verified by exact ProductCode");
      Check(crossProductResult.ExitCode == 1603 && !Directory.Exists(retained.RecoveryDirectory), "verified competitor rollback preserves incoming failure and cleans only its stash");
      Reset(); retained = RemoveLegacy(); NativeRegistrationPresent = false;
      var ghostResult = Recover(retained);
      Check(ghostResult.ExitCode == 1603 && !ghostResult.Message.Contains("automatically restored") && File.Exists(retained.MsiPath),
        "matching ARP identity without Windows Installer registration cannot validate restoration");
      foreach (var marker in new[] { true, false }) {
        Reset(); retained = RemoveLegacy(); NativeRegistrationPresent = false; RestoreMakesRegistration = true;
        Installed = Product(); Installed.IsWindowsInstaller = marker;
        Installed.UninstallString = "msiexec.exe /x " + Code;
        var staleResult = Recover(retained);
        Console.WriteLine("STALE marker=" + marker + " restore=" + Events.Contains("restore") + " restored=" + staleResult.Message.Contains("automatically restored") + " stashExists=" + File.Exists(retained.MsiPath) + " exit=" + staleResult.ExitCode);
        Check(Events.Contains("restore") && staleResult.Message.Contains("automatically restored") && staleResult.ExitCode == 1603,
          "proven unregistered MSI-shaped ARP row does not suppress exact rollback, marker=" + marker);
        Check(!Directory.Exists(retained.RecoveryDirectory), "native-confirmed stale-row recovery cleans only verified restored stash");
      }
      foreach (var mode in new[] { "bad-configuration", "different-user", "advertised", "api-throws", "generic-legacy", "per-user" }) {
        Reset(); retained = RemoveLegacy(); NativeRegistrationPresent = false;
        Installed = Product(Other);
        if (mode == "bad-configuration") NativeProductState = -6;
        if (mode == "different-user") NativeProductState = 2;
        if (mode == "advertised") NativeProductState = 1;
        if (mode == "api-throws") NativeStateThrows = true;
        if (mode == "generic-legacy") { Installed.IsWindowsInstaller = false; Installed.UninstallString = "legacy-uninstaller.exe /S"; }
        if (mode == "per-user") Installed.IsPerUser = true;
        Recover(retained);
        Check(!Events.Contains("restore") && File.Exists(retained.MsiPath), mode + " retains conservative overwrite protection and rollback source");
      }
      Reset(); retained = RemoveLegacy(); RestoredProduct.IsWindowsInstaller = false;
      var markerlessRestore = Recover(retained);
      Check(markerlessRestore.Message.Contains("automatically restored") && !Directory.Exists(retained.RecoveryDirectory),
        "exact native-owned restoration is verified even if its ARP MSI marker is unavailable");
      Reset(); retained = RemoveLegacy(); Installed = Product(); Installed.IsWindowsInstaller = false;
      Installed.UninstallString = "msiexec.exe /x " + Code;
      var ownedMarkerless = Recover(retained);
      Check(!Events.Contains("restore") && ownedMarkerless.ExitCode == 1603 && !Directory.Exists(retained.RecoveryDirectory),
        "native-owned exact product without MSI marker is protected before any repair and recognized without reinstall");
      foreach (var readFailure in new[] { 1, 2 }) {
        foreach (var owned in new[] { false, true }) {
          Reset(); retained = RemoveLegacy();
          if (owned) Installed = Product();
          NativeRegistrationPresent = owned; RegistrationReadFailure = readFailure;
          var inventoryFailure = Recover(retained);
          Check(!Events.Contains("restore") && File.Exists(retained.MsiPath),
            "inventory failure retains exact source and does not authorize repair, owned=" + owned);
          Check(inventoryFailure.ExitCode == 1603 && !inventoryFailure.Message.Contains("automatically restored"),
            "inventory verification failure cannot escape or replace incoming outcome");
        }
      }
      // Vibepollo also protects its own installed product during recovery of
      // the intentionally retained upstream Vibeshine migration source.
      {
        foreach (var code in new[] { Code, Other }) {
          Reset(); retained = RemoveLegacy(); InstalledTarget = Product(code);
          InstalledTarget.Kind = InstallerRunner.InstalledProductKind.Vibepollo;
          Recover(retained);
          Check(!Events.Contains("restore"), "existing downstream product is never overwritten by source restore");
          Check(File.Exists(retained.MsiPath) == (code != Code), "downstream registration cleans stash only for exact restored identity");
        }
        foreach (var code in new[] { Code, Other }) {
          Reset(); retained = RemoveLegacy(); RestoredProduct = null; RestoredTarget = Product(code);
          RestoredTarget.Kind = InstallerRunner.InstalledProductKind.Vibepollo;
          Recover(retained);
          Check(Events.Contains("restore"), "downstream inventory boundary is safe during restore verification");
          Check(File.Exists(retained.MsiPath) == (code != Code), "restore retains source stash when downstream identity differs");
        }
      }
      Console.WriteLine(Checks + " production rollback checks passed (external inventory/MSI/process/security boundaries stubbed; no product installed).");
    } finally { Directory.Delete(Root, true); }
  }
}
