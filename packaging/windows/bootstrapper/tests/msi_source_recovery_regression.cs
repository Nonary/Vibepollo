using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Collections.Generic;
using System.Security.Cryptography;
namespace PRODUCT_NAMESPACE {
  internal static partial class InstallerRunner {
    private const uint MsiErrorMoreData = 234;
    private const int MsiInstallStateDefault = 5;
    internal static string Root, FixtureBundle, Candidate, Stage, Mode;
    internal static bool Elevated, SignatureValid, MetadataValid, IdentityChanges;
    internal static int Runs, Downloads, Registrations, Signatures, ReturnCode, RegistrationCode, Checks;
    internal static List<string> InitialArguments, RetryArguments;
    internal static Dictionary<string,string> NativeProperties;
    internal static List<InstalledProductInfo> FixtureProducts;
    internal static int CleanupCalls, GenericCalls, SecondaryProductState;
    internal static string UnownedProductCode;
    internal static bool SecondaryStateThrows;
    internal static int SecondaryStateChecks, SecondaryOwnershipChecks, KnownDefaultChecks;
    private sealed class RegistryKey : IDisposable {
      public RegistryKey OpenSubKey(string name) { return new RegistryKey(); }
      public void Dispose() { }
      public object Marker;
      public bool ReadThrows;
      public object GetValue(string name) { if (ReadThrows) throw new IOException("unreadable MSI marker"); return Marker; }
    }
    private static bool IsProcessElevated() { return Elevated; }
    private static void AppendInstallerLogMessage(string path, string message) { File.AppendAllText(path, message + "\n"); }
    private static string BuildLogPath(string phase) { return Path.Combine(Root, phase + ".log"); }
    private static string CreateMsiSourceRecoveryDirectory() {
      if (Mode == "directory-failure") throw new IOException("ACL denied");
      Stage = Path.Combine(Root, "stage-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(Stage); return Stage;
    }
    private static void DownloadRecoveryBundle(MsiSourceRelease release, string path) {
      Downloads++;
      if (Mode == "offline") throw new IOException("offline");
      File.Copy(FixtureBundle, path);
      if (Mode == "corrupt-download") File.AppendAllText(path, "bad");
    }
    private static bool VerifyRecoveryBundleSignature(string path) { Signatures++; return SignatureValid; }
    private static MsiSourceIdentity ReadRecoveryMsiIdentity(string path) {
      var expected = RecoverableMsiSources[0].Identity;
      return new MsiSourceIdentity { ProductCode = MetadataValid ? expected.ProductCode : "{22222222-2222-2222-2222-222222222222}",
        PackageCode = expected.PackageCode, Version = expected.Version, Language = expected.Language,
        ProductName = expected.ProductName, UpgradeCode = expected.UpgradeCode, Template = expected.Template };
    }
    private static uint MsiGetProductInfoEx(string product, string sid, uint context, string property, StringBuilder value, ref uint size) {
      Check(context == 4 && sid == null, "machine identity query");
      string item;
      if (!NativeProperties.TryGetValue(property, out item)) return 1608;
      value.Append(item); size = (uint)item.Length;
      if (IdentityChanges && Runs > 0 && property == "PackageCode") value.Append("changed");
      return 0;
    }
    private static uint MsiSourceListAddSourceEx(string product, string sid, uint context, uint options, string source, uint index) {
      Registrations++;
      Check(product == RecoverableMsiSources[0].Identity.ProductCode && sid == null && context == 4 && options == 1 && index == 0,
        "append source for exact machine product only");
      Check(IsVerifiedRecoveryMsi(Path.Combine(source, RecoverableMsiSources[0].MsiName), RecoverableMsiSources[0]), "source registration sees verified MSI");
      return (uint)RegistrationCode;
    }
    private static List<InstalledProductInfo> GetInstalledProducts(bool includeSunshine) { return FixtureProducts.Where(product => product.IsWindowsInstaller).ToList(); }
    private static class Registry {
      internal static readonly RegistryKey LocalMachine = new RegistryKey(), CurrentUser = new RegistryKey();
    }
    private static readonly RegistryKey[] UninstallRegistryHives = { Registry.LocalMachine, Registry.CurrentUser };
    private static readonly string[] UninstallRegistryRoots = { "fixture-uninstall-root" };
    // Registry collection is a supplied leaf. The production getter executes
    // its real facade predicate and identity merge over these exact rows.
    private static void CollectProductRegistrationsFromRoot(RegistryKey hive, RegistryKey root, string path,
      List<InstalledProductInfo> rows, bool includeSunshine) {
      rows.AddRange(FixtureProducts.Where(product => product.IsPerUser == (hive == Registry.CurrentUser)
        && (includeSunshine || product.Kind != InstalledProductKind.Sunshine)));
    }
    private static string TryGetProductLocalPackagePath(string code) {
      if (code == UnownedProductCode) { SecondaryOwnershipChecks++; return null; }
      return "controlled-owned-cache.msi";
    }
    private static int MsiQueryProductState(string code) {
      if (code == RecoverableMsiSources[0].Identity.ProductCode) {
        KnownDefaultChecks++;
        string value; int state;
        return NativeProperties.TryGetValue("ProductState", out value) && int.TryParse(value, out state) ? state : -1;
      }
      SecondaryStateChecks++;
      if (SecondaryStateThrows) throw new IOException("mock native state unavailable");
      return SecondaryProductState;
    }
    private static string ReadCachedMsiInstallRoot(string source, string code, Func<string,string,string> componentPath) {
      return Mode == "unknown-root" ? null : Path.Combine(Root, "custom install root");
    }
    private static string ReadRegisteredMsiComponentPath(string product, string component) { return null; }
    private static bool TryCreateSecureInstallerRecoveryDirectory(out string directory, out string error) {
      directory = Path.Combine(Root, "rollback-" + Guid.NewGuid().ToString("N")); error = ""; Directory.CreateDirectory(directory); return true;
    }
    private static void TryDeleteInstallerRecoveryDirectory(string directory) { Directory.Delete(directory, true); }
    private static bool CanUninstallProduct(InstalledProductInfo product) { return true; }
    private static void TryStopRelatedServicesAndProcesses(string log) { }
    private static void CleanupStaleComponentClientsForInstallLocation(string root, string log) { }
    private static bool IsRecoverableMsiFirewallCleanupFailure(int code, string log) { return false; }
    private static bool TryRunFirewallTolerantUninstall(InstalledProductInfo product, IReadOnlyList<string> args, string log, string phase,
      bool hidden, bool elevate, bool preserve, out int retry, out string retryLog, out string detail, out StashedVibeshinePayload stash) {
      retry = 1603; retryLog = detail = ""; stash = null; throw new Exception("unexpected firewall recovery");
    }
    private static void CleanupCustomArpRegistration(string root, string log) { CleanupCalls++; }
    private static void ScheduleSelfDeleteAndEmptyInstallRootCleanup(string root, string log) { CleanupCalls++; }
    private static void TryFactoryResetKnownAppData(string root) { throw new Exception("unexpected configuration reset"); }
    private static bool InstallLogIndicatesDriverRebootRequired(string log) { return false; }
    private static List<string> CollectVirtualGamepadCleanupWarnings(string log) { return new List<string>(); }
    private static string BuildResultMessage(string operation, int code, string log) { return operation + " " + code; }
    private static string BuildProductUninstallFailureMessage(InstalledProductInfo product, int code, string log) { return "Uninstall " + code; }
    private static int RunUninstallCommand(InstalledProductInfo product, bool hidden, bool elevate) { GenericCalls++; return 0; }
    private static void AdoptStashedPayload(ref StashedVibeshinePayload selected, StashedVibeshinePayload candidate) {
      if (candidate == null) return;
      Check(selected == null, "one exact legacy rollback owner"); selected = candidate;
    }
    private static int RunMsiexec(IReadOnlyList<string> args, bool hidden, bool elevate) {
      Runs++;
      if (Runs == 1) { InitialArguments = args.ToList(); return ReturnCode; }
      RetryArguments = args.ToList(); return Mode == "retry-failure" ? 1603 : Mode == "retry-reboot" ? 3010 : 0;
    }
    private static void Check(bool condition, string reason) { Checks++; if (!condition) throw new Exception(reason); }
    private static string Hash(byte[] bytes) { return Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(); }
    private static InstalledProductInfo Product() { return new InstalledProductInfo {
      ProductCode = RecoverableMsiSources[0].Identity.ProductCode, DisplayName = "Vibepollo", Kind = InstalledProductKind.Vibepollo, IsWindowsInstaller = true };
    }
    private static void Reset(string mode = "missing") {
      Root = Path.Combine(Path.GetTempPath(), "msi-source-behavior-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(Root);
      Mode = mode; Stage = null; CleanupCalls = GenericCalls = 0; FixtureProducts = new List<InstalledProductInfo> { Product() }; Runs = Downloads = Registrations = Signatures = RegistrationCode = 0;
      UnownedProductCode = null; SecondaryProductState = -1; SecondaryStateThrows = false;
      SecondaryStateChecks = SecondaryOwnershipChecks = KnownDefaultChecks = 0;
      Elevated = SignatureValid = MetadataValid = true; IdentityChanges = false; ReturnCode = 1612;
      InitialArguments = RetryArguments = null;
      MsiSourceRecoveryAttempts.Clear(); MsiSourceRecoveryRetries.Clear(); RecoveredMsiSources.Clear();
      var release = RecoverableMsiSources[0];
      var payload = Encoding.UTF8.GetBytes("isolated MSI payload fixture: no Windows installation");
      var bundle = new byte[payload.Length + 20];
      BitConverter.GetBytes((uint)payload.Length).CopyTo(bundle, 12); payload.CopyTo(bundle, 16);
      release.PayloadOffset = 16; release.PayloadSize = payload.Length; release.BundleSize = bundle.Length;
      release.BundleSha256 = Hash(bundle); release.MsiSha256 = Hash(payload);
      FixtureBundle = Path.Combine(Root, "fixture.exe"); File.WriteAllBytes(FixtureBundle, bundle);
      Candidate = Path.Combine(Root, release.MsiName);
      NativeProperties = new Dictionary<string,string> {
        {"ProductState", "5"}, {"Transforms", ""}, {"PackageCode", release.Identity.PackageCode}, {"VersionString", release.Identity.Version},
        {"Language", release.Identity.Language}, {"InstalledProductName", release.Identity.ProductName}, {"InstallSource", Root}
      };
    }
    private static int Execute(InstalledProductInfo product = null) {
      string log = Path.Combine(Root, "initial.log");
      var args = new List<string> { "/x", RecoverableMsiSources[0].Identity.ProductCode, "/qn", "/norestart", "/l*v", log,
        "FACTORYRESET=0", "REMOVEVIRTUALDISPLAYDRIVER=0", "UPGRADINGPRODUCTCODE=1", "REBOOT=ReallySuppress" };
      var result = RunMsiexecWithSourceRecovery(args, true, false, product ?? Product(), ref log);
      if (Runs == 2) {
        Check(args.SequenceEqual(InitialArguments), "initial arguments are unchanged");
        var expected = args.ToList(); expected[5] = log;
        Check(expected.SequenceEqual(RetryArguments), "retry preserves target, upgrade, config, driver and reboot properties");
        Check(log != args[5] && File.ReadAllText(args[5]).Contains(log), "initial evidence points to separate retry log");
      }
      return result;
    }
    private static void Cleanup() { Directory.Delete(Root, true); }
    public static void Main() {
      foreach (var mode in new[] {"missing", "corrupt-local", "wrong-version-local", "offline", "corrupt-download", "signature-failure", "metadata-failure", "directory-failure", "registration-failure", "retry-failure", "retry-reboot", "identity-change", "good-local-offline"}) {
        Reset(mode);
        try {
          if (mode == "corrupt-local") File.WriteAllText(Candidate, "corrupt cache");
          if (mode == "wrong-version-local") File.WriteAllText(Candidate, "a different complete version");
          if (mode == "good-local-offline") File.WriteAllBytes(Candidate, File.ReadAllBytes(FixtureBundle).Skip(16).Take((int)RecoverableMsiSources[0].PayloadSize).ToArray());
          if (mode == "signature-failure") SignatureValid = false;
          if (mode == "metadata-failure") MetadataValid = false;
          if (mode == "registration-failure") RegistrationCode = 5;
          if (mode == "identity-change") IdentityChanges = true;
          var originalCandidate = File.Exists(Candidate) ? File.ReadAllBytes(Candidate) : null;
          var actual = Execute();
          bool sourceRestored = new[] {"missing", "corrupt-local", "wrong-version-local", "retry-failure", "retry-reboot", "good-local-offline"}.Contains(mode);
          Check(actual == (sourceRestored ? mode == "retry-failure" ? 1603 : mode == "retry-reboot" ? 3010 : 0 : 1612), mode + " preserves actual outcome");
          Check(Runs == (sourceRestored ? 2 : 1), mode + " bounded operation attempts");
          Check(!sourceRestored || File.Exists(Path.Combine(Stage, RecoverableMsiSources[0].MsiName)), mode + " retains registered source for maintenance/rollback");
          Check(sourceRestored || Stage == null || !Directory.Exists(Stage), mode + " cleans own failed staging only");
          Check(originalCandidate == null || File.ReadAllBytes(Candidate).SequenceEqual(originalCandidate), mode + " never deletes or replaces original source cache");
          if (mode == "good-local-offline") Check(Downloads == 0 && Signatures == 0, "pinned original MSI works without download");
          else if (mode != "directory-failure") Check(Downloads == 1, mode + " one acquisition attempt");
          Console.WriteLine("PASS " + mode);
        } finally { Cleanup(); }
      }
      foreach (var field in new[] {"PackageCode", "VersionString", "Language", "InstalledProductName", "Transforms", "missing-field", "short-version", "per-user", "unsupported-kind", "not-msi", "not-elevated"}) {
        Reset();
        try {
          var product = Product();
          if (field == "short-version") NativeProperties["VersionString"] = "1.18";
          else if (field == "missing-field") NativeProperties.Remove("PackageCode");
          else if (field == "per-user") product.IsPerUser = true;
          else if (field == "unsupported-kind") product.Kind = InstalledProductKind.Apollo;
          else if (field == "not-msi") product.IsWindowsInstaller = false;
          else if (field == "not-elevated") Elevated = false;
          else NativeProperties[field] += "wrong";
          Check(Execute(product) == 1612 && Runs == 1 && Downloads == 0 && Registrations == 0 && Stage == null, field + " fails before mutation/acquisition");
        } finally { Cleanup(); }
      }
      foreach (var mode in new[] { "standalone", "offline", "retry-failure", "retry-reboot" }) {
        Reset(mode);
        try {
          var result = UninstallInstalledProducts("uninstall", true, false, false, false, true, new[] { InstalledProductKind.Vibepollo });
          var expected = mode == "offline" ? 1612 : mode == "retry-failure" ? 1603 : mode == "retry-reboot" ? 3010 : 0;
          Check(result.ExitCode == expected && Runs == (mode == "offline" ? 1 : 2), "real standalone uninstall executes bounded source recovery: " + mode);
          Check(CleanupCalls == (expected == 0 || expected == 3010 ? 2 : 0), "standalone failure cannot trigger success cleanup: " + mode);
        } finally { Cleanup(); }
      }
      foreach (var mode in new[] { "replacement", "offline", "unknown-root", "ambiguous", "identity-missing-field", "identity-transforms", "identity-mismatch" }) {
        Reset(mode);
        try {
          if (mode == "ambiguous") FixtureProducts.Add(new InstalledProductInfo { ProductCode = "{22222222-2222-2222-2222-222222222222}", IsWindowsInstaller = true, Kind = InstalledProductKind.Vibeshine });
          if (mode == "identity-missing-field") NativeProperties.Remove("PackageCode");
          if (mode == "identity-transforms") NativeProperties["Transforms"] = "unreadable-or-transformed";
          if (mode == "identity-mismatch") NativeProperties["PackageCode"] = "{22222222-2222-2222-2222-222222222222}";
          StashedVibeshinePayload stash;
          ReturnCode = 0;
          var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
          if (mode == "replacement") {
            Check(result.Succeeded && Runs == 1 && stash != null, "real replacement dispatcher preserves source before uninstall");
            Check(IsVerifiedRecoveryMsi(stash.MsiPath, RecoverableMsiSources[0]), "rollback stash holds exact full original source");
            Check(stash.InstallLocation == Path.Combine(Root, "custom install root"), "recover missing-cache installation root from registered component path");
          } else Check(!result.Succeeded && Runs == 0 && stash == null && CleanupCalls == 0, mode + " stops replacement before uninstall without rollback source");
        } finally { Cleanup(); }
      }
      foreach (var field in new[] { "Transforms", "PackageCode", "VersionString", "Language", "InstalledProductName" }) {
        Reset();
        try {
          NativeProperties.Remove(field); ReturnCode = 0;
          StashedVibeshinePayload stash;
          var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
          Check(!result.Succeeded && Runs == 0 && Downloads == 0 && Registrations == 0 && stash == null,
            "known ProductCode with unreadable " + field + " cannot reach destructive replacement");
        } finally { Cleanup(); }
      }
      Reset();
      try {
        FixtureProducts[0].ProductCode = "{22222222-2222-2222-2222-222222222222}"; ReturnCode = 0;
        StashedVibeshinePayload stash;
        var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
        Check(result.Succeeded && Runs == 1 && Downloads == 0 && Registrations == 0 && stash == null,
          "other ProductCode keeps normal successful replacement behavior");
      } finally { Cleanup(); }
      // Run the production registry-marker classifier and real replacement
      // dispatcher; only final generic/MSI process boundaries are inert.
      foreach (var marker in new object[] { 0, "0", null, "invalid", "throw", 1, "1" }) {
        Reset();
        try {
          var row = FixtureProducts[0];
          row.IsWindowsInstaller = IsWindowsInstallerProduct(new RegistryKey { Marker = marker, ReadThrows = object.Equals(marker, "throw") });
          row.UninstallString = "msiexec.exe /x " + row.ProductCode;
          NativeProperties.Remove("PackageCode"); ReturnCode = 0;
          StashedVibeshinePayload stash;
          var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
          Console.WriteLine("MARKER " + (marker ?? "missing") + " result=" + result.ExitCode + " msi=" + Runs + " generic=" + GenericCalls + " stash=" + (stash != null));
          Check(result.ExitCode == 1612 && Runs == 0 && GenericCalls == 0 && CleanupCalls == 0 && stash == null,
            "incomplete known identity blocks every marker before generic/native dispatch: " + (marker ?? "missing"));
        } finally { Cleanup(); }
      }
      foreach (var marker in new object[] { 0, null, "throw" }) {
        Reset();
        try {
          var row = FixtureProducts[0];
          row.IsWindowsInstaller = IsWindowsInstallerProduct(new RegistryKey { Marker = marker, ReadThrows = object.Equals(marker, "throw") });
          row.UninstallString = "arbitrary-generic-uninstaller.exe /S";
          row.InstallLocation = Path.Combine(Root, "untrusted ARP root");
          ReturnCode = 0;
          StashedVibeshinePayload stash;
          var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
          Check(result.Succeeded && Runs == 1 && GenericCalls == 0 && stash != null,
            "native-verified known product ignores malformed marker/generic command and uses MSI: " + (marker ?? "missing"));
          Check(stash.InstallLocation == Path.Combine(Root, "custom install root") && IsVerifiedRecoveryMsi(stash.MsiPath, RecoverableMsiSources[0]),
            "marker recovery preserves verified full source and native registered root");
          Check(InitialArguments[0] == "/x" && InitialArguments[1] == row.ProductCode && InitialArguments.Contains("UPGRADINGPRODUCTCODE=1"),
            "marker recovery dispatch retains exact MSI identity and upgrade policy");
          Check(!row.IsWindowsInstaller && row.InstallLocation.EndsWith("untrusted ARP root"), "repair does not rewrite original inventory or ARP metadata");
        } finally { Cleanup(); }
      }
      foreach (var state in new string[] { null, "1", "2", "-1", "-6" }) {
        Reset();
        try {
          FixtureProducts[0].IsWindowsInstaller = false;
          if (state == null) NativeProperties.Remove("ProductState"); else NativeProperties["ProductState"] = state;
          ReturnCode = 0;
          StashedVibeshinePayload stash;
          var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
          Check(!result.Succeeded && Runs == 0 && GenericCalls == 0 && Downloads == 0 && Registrations == 0 && stash == null,
            "unverified/noninstalled native state cannot promote a generic marker: " + (state ?? "unreadable"));
        } finally { Cleanup(); }
      }
      Reset("offline");
      try {
        FixtureProducts[0].IsWindowsInstaller = false; ReturnCode = 0;
        StashedVibeshinePayload stash;
        var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
        Check(!result.Succeeded && Runs == 0 && GenericCalls == 0 && stash == null, "offline malformed-marker recovery stops before removal");
      } finally { Cleanup(); }
      foreach (var code in new[] { "", "{22222222-2222-2222-2222-222222222222}" }) {
        Reset();
        try {
          var row = FixtureProducts[0]; row.ProductCode = code; row.IsWindowsInstaller = false;
          row.UninstallString = "msiexec.exe /x " + RecoverableMsiSources[0].Identity.ProductCode;
          ReturnCode = 0;
          StashedVibeshinePayload stash;
          var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
          Check(!result.Succeeded && Runs == 0 && GenericCalls == 0 && stash == null,
            "generic command cannot name known package through missing/different row identity");
        } finally { Cleanup(); }
      }
      // Execute the actual resolver and replacement caller together. A
      // markerless known GUID must survive normalization, and all known
      // guards must run before an earlier inventory row can be removed.
      foreach (var marker in new object[] { 0, null, "throw" }) {
        foreach (var knownFirst in new[] { false, true }) {
          Reset();
          try {
            var known = FixtureProducts[0];
            known.IsWindowsInstaller = IsWindowsInstallerProduct(new RegistryKey { Marker = marker, ReadThrows = object.Equals(marker, "throw") });
            known.InstallLocation = Path.Combine(Root, "shared root");
            known.UninstallString = "arbitrary-generic.exe /S";
            var other = new InstalledProductInfo { ProductCode = "{22222222-2222-2222-2222-222222222222}",
              Kind = InstalledProductKind.Vibepollo, IsWindowsInstaller = true, InstallLocation = known.InstallLocation };
            FixtureProducts = knownFirst ? new List<InstalledProductInfo> { known, other } : new List<InstalledProductInfo> { other, known };
            ReturnCode = 0;
            StashedVibeshinePayload stash;
            var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
            Check(result.ExitCode == 1612 && Runs == 0 && GenericCalls == 0 && stash == null,
              "resolver-to-dispatch ambiguous known identity blocks all removals, order=" + knownFirst + " marker=" + (marker ?? "missing"));
            Check(GetInstalledProductRegistrations(false).Any(row => row.ProductCode == known.ProductCode),
              "facade normalization preserves catalogued markerless ProductCode");
          } finally { Cleanup(); }
        }
      }
      foreach (var marker in new object[] { 0, null, "throw" }) {
        foreach (var rowCode in new[] { "", "{33333333-3333-3333-3333-333333333333}" }) {
          foreach (var quiet in new[] { false, true }) {
            Reset();
            try {
              var row = FixtureProducts[0]; row.ProductCode = rowCode;
              row.IsWindowsInstaller = IsWindowsInstallerProduct(new RegistryKey { Marker = marker, ReadThrows = object.Equals(marker, "throw") });
              row.InstallLocation = Path.Combine(Root, "shared facade root");
              var command = "msiexec.exe /x " + RecoverableMsiSources[0].Identity.ProductCode;
              if (quiet) row.QuietUninstallString = command; else row.UninstallString = command;
              var other = new InstalledProductInfo { ProductCode = "{22222222-2222-2222-2222-222222222222}",
                Kind = InstalledProductKind.Vibepollo, IsWindowsInstaller = true, InstallLocation = row.InstallLocation };
              FixtureProducts = new List<InstalledProductInfo> { other, row }; ReturnCode = 0;
              StashedVibeshinePayload stash;
              var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
              Check(result.ExitCode == 1612 && Runs == 0 && GenericCalls == 0 && Downloads == 0 && stash == null,
                "resolver preserves known-command mismatched association before any dispatch, marker=" + (marker ?? "missing") + " quiet=" + quiet);
            } finally { Cleanup(); }
          }
        }
      }
      foreach (var secondaryState in new[] { -6, 1, 2, -99 }) {
        Reset();
        try {
          var other = new InstalledProductInfo { ProductCode = "{22222222-2222-2222-2222-222222222222}",
            Kind = InstalledProductKind.Vibeshine, IsWindowsInstaller = true };
          FixtureProducts.Insert(0, other); UnownedProductCode = other.ProductCode;
          SecondaryProductState = secondaryState; SecondaryStateThrows = secondaryState == -99;
          StashedVibeshinePayload stash;
          Check(!TryPreserveKnownMsiSourceBeforeReplacement(FixtureProducts[1], BuildLogPath("uncertain_secondary"), out stash)
            && stash == null && Downloads == 0 && Runs == 0,
            "uncertain secondary native guard fails closed before source acquisition, state=" + secondaryState);
          var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
          Check(result.ExitCode == 1612 && Runs == 0 && GenericCalls == 0 && Downloads == 0 && stash == null,
            "uncertain secondary native identity cannot disappear from preservation ambiguity, state=" + secondaryState);
        } finally { Cleanup(); }
      }
      Reset();
      try {
        var other = new InstalledProductInfo { ProductCode = "{22222222-2222-2222-2222-222222222222}",
          Kind = InstalledProductKind.Vibeshine, IsWindowsInstaller = true };
        FixtureProducts.Add(other); UnownedProductCode = other.ProductCode; SecondaryProductState = -1;
        StashedVibeshinePayload stash;
        Check(TryPreserveKnownMsiSourceBeforeReplacement(FixtureProducts[0], BuildLogPath("definitively_absent"), out stash)
          && stash != null && Runs == 0, "definitively UNKNOWN secondary identity permits verified preservation");
      } finally { Cleanup(); }
      // Full production getter -> resolver -> preservation -> dispatcher.
      // A is native DEFAULT under the getter's actual liveness predicate.
      foreach (var marker in new object[] { 1, 0, null, "throw" }) {
        foreach (var knownFirst in new[] { false, true }) {
          foreach (var state in new[] { -6, 5, 1, -99 }) {
            Reset();
            try {
              var known = FixtureProducts[0]; known.InstallLocation = Path.Combine(Root, "shared root");
              var secondary = new InstalledProductInfo { ProductCode = "{22222222-2222-2222-2222-222222222222}",
                DisplayName = "Vibepollo", Kind = InstalledProductKind.Vibepollo,
                IsWindowsInstaller = IsWindowsInstallerProduct(new RegistryKey { Marker = marker, ReadThrows = object.Equals(marker, "throw") }),
                InstallLocation = known.InstallLocation, UninstallString = "legacy-custom.exe /S" };
              FixtureProducts = knownFirst ? new List<InstalledProductInfo> { known, secondary } : new List<InstalledProductInfo> { secondary, known };
              UnownedProductCode = secondary.ProductCode; SecondaryProductState = state; SecondaryStateThrows = state == -99; ReturnCode = 0;
              var visible = GetInstalledProductRegistrations(false);
              Check(visible.Count == 2 && visible.Any(row => row.ProductCode == secondary.ProductCode),
                "secondary GUID survives normalization before ambiguity preflight, marker=" + (marker ?? "missing") + " state=" + state);
              StashedVibeshinePayload stash;
              var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
              Check(result.ExitCode == 1612 && Runs == 0 && GenericCalls == 0 && Downloads == 0 && Registrations == 0 && stash == null,
                "secondary owned/nonUNKNOWN/unreadable identity blocks every dispatch, marker=" + (marker ?? "missing") + " state=" + state + " knownFirst=" + knownFirst);
              Check(SecondaryStateChecks > 0, "secondary native state is actually probed before dispatch");
              Console.WriteLine("PASS secondary marker=" + (marker ?? "missing") + " state=" + state + " order=" + knownFirst
                + " visible=" + visible.Count + " result=" + result.ExitCode + " msi=" + Runs + " generic=" + GenericCalls);
            } finally { Cleanup(); }
          }
        }
      }
      foreach (var marker in new object[] { 0, null, "throw" }) {
        foreach (var knownFirst in new[] { false, true }) {
          Reset();
          try {
            var known = FixtureProducts[0]; known.InstallLocation = Path.Combine(Root, "shared root");
            var facade = new InstalledProductInfo { ProductCode = "{22222222-2222-2222-2222-222222222222}",
              DisplayName = "Vibepollo", Kind = InstalledProductKind.Vibepollo,
              IsWindowsInstaller = IsWindowsInstallerProduct(new RegistryKey { Marker = marker, ReadThrows = object.Equals(marker, "throw") }),
              InstallLocation = known.InstallLocation, UninstallString = "legacy-custom.exe /S" };
            FixtureProducts = knownFirst ? new List<InstalledProductInfo> { known, facade } : new List<InstalledProductInfo> { facade, known };
            UnownedProductCode = facade.ProductCode; SecondaryProductState = -1; ReturnCode = 0;
            var visible = GetInstalledProductRegistrations(false);
            Check(visible.Count == 1 && visible[0].ProductCode == known.ProductCode && SecondaryStateChecks >= 2 && SecondaryOwnershipChecks > 0,
              "definitively absent UNKNOWN GUID facade still collapses after both native probes");
            Check(KnownDefaultChecks > 0, "production getter uses native DEFAULT liveness for known A");
            StashedVibeshinePayload stash;
            var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
            Check(result.Succeeded && Runs == 1 && GenericCalls == 0 && stash != null && InitialArguments[1] == known.ProductCode,
              "genuine UNKNOWN facade permits exactly the preserved known MSI operation");
            Console.WriteLine("PASS UNKNOWN facade marker=" + (marker ?? "missing") + " order=" + knownFirst + " result=" + result.ExitCode);
          } finally { Cleanup(); }
        }
      }
      foreach (var knownFirst in new[] { false, true }) {
        Reset();
        try {
          var known = FixtureProducts[0]; known.InstallLocation = Path.Combine(Root, "shared root");
          var facade = new InstalledProductInfo { ProductCode = "", Kind = InstalledProductKind.Vibepollo,
            InstallLocation = known.InstallLocation, UninstallString = "legacy-custom.exe /S" };
          FixtureProducts = knownFirst ? new List<InstalledProductInfo> { known, facade } : new List<InstalledProductInfo> { facade, known };
          ReturnCode = 0;
          var visible = GetInstalledProductRegistrations(false);
          Check(visible.Count == 1 && visible[0].ProductCode == known.ProductCode && KnownDefaultChecks > 0,
            "nonGUID custom executable facade still resolves through actual DEFAULT predicate");
          StashedVibeshinePayload stash;
          var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
          Check(result.Succeeded && Runs == 1 && GenericCalls == 0 && stash != null,
            "ordinary custom executable facade does not disable valid replacement");
        } finally { Cleanup(); }
      }
      // Combined marker matrix through the actual full getter. The known
      // machine A is DEFAULT; only a definitively absent UNKNOWN GUID B may
      // collapse, and no generic dispatch may follow exact known routing.
      foreach (var aMarker in new object[] { 1, 0, null, "throw" }) {
        foreach (var bMarker in new object[] { 1, 0, null, "throw" }) {
          foreach (var knownFirst in new[] { false, true }) {
            foreach (var unsafeState in new[] { "owned-unknown", "badconfig", "live", "advertised", "query-error" }) {
              Reset();
              try {
                var known = FixtureProducts[0];
                known.IsWindowsInstaller = IsWindowsInstallerProduct(new RegistryKey { Marker = aMarker, ReadThrows = object.Equals(aMarker, "throw") });
                known.InstallLocation = Path.Combine(Root, "custom install root");
                var secondary = new InstalledProductInfo { ProductCode = "{22222222-2222-2222-2222-222222222222}",
                  DisplayName = "Vibepollo", Kind = InstalledProductKind.Vibepollo,
                  IsWindowsInstaller = IsWindowsInstallerProduct(new RegistryKey { Marker = bMarker, ReadThrows = object.Equals(bMarker, "throw") }),
                  InstallLocation = known.InstallLocation, UninstallString = "legacy-custom.exe /S" };
                FixtureProducts = knownFirst ? new List<InstalledProductInfo> { known, secondary } : new List<InstalledProductInfo> { secondary, known };
                UnownedProductCode = unsafeState == "owned-unknown" ? null : secondary.ProductCode;
                SecondaryProductState = unsafeState == "badconfig" ? -6 : unsafeState == "live" ? 5 : unsafeState == "advertised" ? 1 : -1;
                SecondaryStateThrows = unsafeState == "query-error"; ReturnCode = 0;
                var visible = GetInstalledProductRegistrations(false);
                Check(visible.Count == 2 && visible.Any(row => row.ProductCode == secondary.ProductCode), "combined markers retain unsafe secondary GUID");
                StashedVibeshinePayload stash;
                var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
                Check(result.ExitCode == 1612 && Runs == 0 && GenericCalls == 0 && stash == null && Downloads == 0 && Registrations == 0,
                  "combined markers cannot dispatch through secondary ownership/state uncertainty: " + unsafeState);
              } finally { Cleanup(); }
            }
          }
        }
        foreach (var bMarker in new object[] { 0, null, "throw" }) {
          foreach (var knownFirst in new[] { false, true }) {
            foreach (var facadeCode in new[] { "{22222222-2222-2222-2222-222222222222}", "" }) {
              Reset();
              try {
                var known = FixtureProducts[0];
                known.IsWindowsInstaller = IsWindowsInstallerProduct(new RegistryKey { Marker = aMarker, ReadThrows = object.Equals(aMarker, "throw") });
                known.InstallLocation = Path.Combine(Root, "custom install root");
                var facade = new InstalledProductInfo { ProductCode = facadeCode, DisplayName = "Vibepollo", Kind = InstalledProductKind.Vibepollo,
                  IsWindowsInstaller = IsWindowsInstallerProduct(new RegistryKey { Marker = bMarker, ReadThrows = object.Equals(bMarker, "throw") }),
                  InstallLocation = known.InstallLocation, UninstallString = "legacy-custom.exe /S" };
                FixtureProducts = knownFirst ? new List<InstalledProductInfo> { known, facade } : new List<InstalledProductInfo> { facade, known };
                UnownedProductCode = facade.ProductCode; SecondaryProductState = -1; ReturnCode = 0;
                var visible = GetInstalledProductRegistrations(false);
                Check(visible.Count == 1 && visible[0].ProductCode == known.ProductCode && KnownDefaultChecks > 0,
                  "catalogued DEFAULT A absorbs genuine facade independently of both ARP markers");
                if (facadeCode.Length != 0) Check(SecondaryStateChecks > 0 && SecondaryOwnershipChecks > 0, "GUID facade requires native absent plus UNKNOWN");
                StashedVibeshinePayload stash;
                var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
                Check(result.Succeeded && Runs == 1 && GenericCalls == 0 && stash != null && InitialArguments[1] == known.ProductCode,
                  "marker-independent known routing performs exact MSI once with no extra generic boundary");
              } finally { Cleanup(); }
            }
          }
        }
        foreach (var failure in new[] { "non-default", "identity-mismatch", "missing-identity", "per-user", "offline" }) {
          foreach (var knownFirst in new[] { false, true }) {
            Reset(failure == "offline" ? "offline" : "missing");
            try {
              var known = FixtureProducts[0];
              known.IsWindowsInstaller = IsWindowsInstallerProduct(new RegistryKey { Marker = aMarker, ReadThrows = object.Equals(aMarker, "throw") });
              known.InstallLocation = Path.Combine(Root, "custom install root");
              if (failure == "non-default") NativeProperties["ProductState"] = "1";
              if (failure == "identity-mismatch") NativeProperties["PackageCode"] += "wrong";
              if (failure == "missing-identity") NativeProperties.Remove("PackageCode");
              if (failure == "per-user") known.IsPerUser = true;
              var facade = new InstalledProductInfo { ProductCode = "{22222222-2222-2222-2222-222222222222}",
                Kind = InstalledProductKind.Vibepollo, InstallLocation = known.InstallLocation, UninstallString = "legacy-custom.exe /S" };
              FixtureProducts = knownFirst ? new List<InstalledProductInfo> { known, facade } : new List<InstalledProductInfo> { facade, known };
              UnownedProductCode = facade.ProductCode; SecondaryProductState = -1; ReturnCode = 0;
              StashedVibeshinePayload stash;
              var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
              Check(result.ExitCode == 1612 && Runs == 0 && GenericCalls == 0 && stash == null,
                "facade candidate never bypasses known native identity/source/machine preflight: " + failure);
            } finally { Cleanup(); }
          }
        }
      }
      foreach (var knownFirst in new[] { false, true }) {
        foreach (var authority in new[] { "foreign-code", "command-only", "different-kind", "different-root" }) {
          Reset();
          try {
            var candidate = FixtureProducts[0]; candidate.IsWindowsInstaller = false;
            candidate.InstallLocation = Path.Combine(Root, "custom install root");
            var facade = new InstalledProductInfo { ProductCode = "{22222222-2222-2222-2222-222222222222}",
              Kind = InstalledProductKind.Vibepollo, InstallLocation = candidate.InstallLocation, UninstallString = "legacy-custom.exe /S" };
            if (authority == "foreign-code") candidate.ProductCode = "{33333333-3333-3333-3333-333333333333}";
            if (authority == "command-only") { candidate.ProductCode = ""; candidate.UninstallString = "msiexec.exe /x " + RecoverableMsiSources[0].Identity.ProductCode; }
            if (authority == "different-kind") candidate.Kind = InstalledProductKind.Vibeshine;
            if (authority == "different-root") candidate.InstallLocation = Path.Combine(Root, "different root");
            FixtureProducts = knownFirst ? new List<InstalledProductInfo> { candidate, facade } : new List<InstalledProductInfo> { facade, candidate };
            UnownedProductCode = facade.ProductCode; SecondaryProductState = -1; ReturnCode = 0;
            var visible = GetInstalledProductRegistrations(false);
            Check(visible.Count == 2 && visible.Any(row => row.ProductCode == facade.ProductCode),
              "catalogue matching retains kind/root/GUID authority boundary: " + authority);
            if (authority == "command-only") {
              StashedVibeshinePayload stash;
              var result = UninstallCompetingProducts("install_remove_competing", true, false, out stash);
              Check(result.ExitCode == 1612 && Runs == 0 && GenericCalls == 0 && stash == null,
                "arbitrary known-code command is never native MSI candidate authority");
            }
          } finally { Cleanup(); }
        }
      }
      Reset("retry-failure");
      try {
        Check(Execute() == 1603, "retry failure returned");
        Runs = 0;
        Check(Execute() == 1612 && Runs == 1 && Downloads == 1 && Registrations == 1, "same process never retries recovery twice");
        var log = Path.Combine(Root, "registration.log");
        string detail;
        Check(!TryRepairBustedMsiRegistration(new InstallerResult { ExitCode = 1612, LogPath = log }, MsiRegistrationRecoveryKinds, "test", out detail), "cache error cannot authorize registration deletion");
      } finally { Cleanup(); }
      Reset();
      try {
        ReturnCode = 0; Check(Execute() == 0 && Runs == 1 && Downloads == 0, "successful first operation performs no recovery");
        ReturnCode = 5; Runs = 0; Check(Execute() == 5 && Runs == 1 && Downloads == 0, "unrelated failure performs no recovery");
        var release = RecoverableMsiSources[0]; release.PayloadOffset = long.MaxValue;
        try { ExtractVerifiedRecoveryBundle(release, FixtureBundle, Candidate); throw new Exception("invalid bounds accepted"); }
        catch (IOException) { Check(!File.Exists(Candidate), "bad resource bounds rejected before writing MSI"); }
      } finally { Cleanup(); }
      Console.WriteLine("PASS " + Checks + " behavioral checks; Windows/native boundaries mocked.");
    }
  }
}
