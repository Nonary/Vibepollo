using System;
using System.IO;
using System.Linq;
using PRODUCT_NAMESPACE;

internal static class MigrationRegression {
  private static int checks;
  private const string Code = "{11111111-1111-1111-1111-111111111111}";
  private const string Other = "{22222222-2222-2222-2222-222222222222}";
  private static void Check(bool value, string name) { if (!value) throw new Exception(name); checks++; }
  private static void Reject(Action action, string name) {
    try { action(); } catch (InvalidOperationException) { checks++; return; }
    throw new Exception(name);
  }
  private static InstallerRunner.InstalledProductInfo Product(bool msi, string path, string code = Code, bool perUser = false) {
    return new InstallerRunner.InstalledProductInfo { IsWindowsInstaller = msi, IsPerUser = perUser,
      Kind = InstallerRunner.InstalledProductKind.Vibepollo, ProductCode = msi ? code : "",
      RegistryPath = msi ? "HKLM\\MSI\\" + code : "HKLM\\ARP\\Vibepollo", InstallLocation = path,
      UninstallString = "\"" + Path.Combine(path, "uninstall.exe") + "\"", Version = new Version(2, 0, 90) };
  }
  private static InstallerRunner.PayloadMsiInfo Metadata(string code, string upgrade, bool transactional = true) {
    return new InstallerRunner.PayloadMsiInfo { ProductCode = code, UpgradeCode = upgrade, SupportsTransactionalReplacement = transactional };
  }
  private static void Write(string root, string file, string contents) {
    var path = Path.Combine(root, file); Directory.CreateDirectory(Path.GetDirectoryName(path)); File.WriteAllText(path, contents);
  }
  private static string Read(string root, string file) { return File.ReadAllText(Path.Combine(root, file)); }
  public static void Main(string[] args) {
    var root = Path.Combine(Path.GetTempPath(), "bootstrap migration ' & Unicode-日本 " + Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try {
      var source = Path.Combine(root, "old host"); var target = Path.Combine(root, "new host"); var recovery = Path.Combine(root, "checkpoints");
      Directory.CreateDirectory(source);
      var facade = Product(false, source + Path.DirectorySeparatorChar); var msi = Product(true, source);
      var plan = InstallerRunner.ResolveBootstrapMigrationProducts(new[] { facade, msi }, code => code == Code);
      Check(plan.Count == 1 && plan[0].IsWindowsInstaller && plan[0].ProductCode == Code, "2.0.0 facade resolves once to its live MSI");
      plan = InstallerRunner.ResolveBootstrapMigrationProducts(new[] { facade, msi }, code => false);
      Check(plan.Count == 2 && plan.Any(p => !p.IsWindowsInstaller), "unregistered MSI does not justify legacy uninstall");
      plan = InstallerRunner.ResolveBootstrapMigrationProducts(new[] { Product(false, source, perUser: true), msi }, code => true);
      Check(plan.Count == 2, "HKCU facade never merged into elevated MSI identity");
      plan = InstallerRunner.ResolveBootstrapMigrationProducts(new[] { facade, Product(true, target) }, code => true);
      Check(plan.Count == 2, "other path never backs facade");
      Reject(() => InstallerRunner.ResolveBootstrapMigrationProducts(new[] { facade, msi, Product(true, source, Other) }, code => true), "ambiguous MSI roots must fail");
      Check(InstallerRunner.ResolveBootstrapMigrationProducts(new InstallerRunner.InstalledProductInfo[0], code => true).Count == 0, "fresh install unchanged");
      Check(InstallerRunner.CanReplaceInMsiTransaction(msi, Metadata(Code, Other), Metadata(Other, Other)), "same family upgrades transactionally");
      Check(!InstallerRunner.CanReplaceInMsiTransaction(msi, Metadata(Code, Other), Metadata(Other, Code)), "cross family is separate migration");
      Check(!InstallerRunner.CanReplaceInMsiTransaction(msi, Metadata(Other, Other), Metadata(Other, Other)), "cache identity must match source");
      Check(!InstallerRunner.CanReplaceInMsiTransaction(msi, Metadata(Code, Other), Metadata(Other, Other, false)), "old incoming payload retains workaround");
      Check(!InstallerRunner.CanReplaceInMsiTransaction(msi, Metadata(Code, Other, false), Metadata(Other, Other)), "unrecognized historical source contract cannot authorize replacement");
      Check(!InstallerRunner.CanReplaceInMsiTransaction(facade, null, Metadata(Other, Other)), "true NSIS has no implicit preservation contract");
      Write(source, "config/sunshine.conf", "unicode = 日本\n"); Write(source, "config/apps.json", "{apps:é}");
      Write(source, "config/sunshine_state.json", "paired-client-secret"); Write(source, "credentials/key.pem", "private-key");
      Write(source, "covers/game.png", "image-bytes"); Write(source, "apps.json", "flat-legacy-apps");
      Write(source, "config/vibeshine_state.json", "display-and-pairing-settings");
      var security = Directory.GetAccessControl(root);
      Reject(() => InstallerRunner.LegacyDataCheckpoint.Open(recovery, Path.GetPathRoot(source), target, Code, security), "volume-root ownership must fail");
      if (args.Length > 0) {
        var queried = false;
        var msiRoot = InstallerRunner.ReadCachedMigrationRoot(args[0], Code, (product, component) => {
          Check(product == Code && component == Other, "cached MSI resolves the exact sunshine executable component");
          queried = true; return Path.Combine(source, "sunshine.exe");
        });
        Check(queried && msiRoot == source, "real MSI database join/record extraction maps installed root");
        Check(InstallerRunner.ReadCachedMigrationRoot(args[0], Code, (product, component) => null) == null, "absent native component cannot invent install root");
        Check(InstallerRunner.CachedMsiPreservesDriversDuringUpgrade(args[0]), "real cached MSI upgrade condition preserves driver removal action");
        Check(args.Length > 1 && !InstallerRunner.CachedMsiPreservesDriversDuringUpgrade(args[1]), "real cached MSI unsafe removal condition blocks migration");
        if (args.Length > 2 && !string.IsNullOrWhiteSpace(args[2])) {
          var flags = System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic;
          var reader = typeof(InstallerRunner).GetMethod("TryGetPayloadMsiInfo", flags, null, new[] { typeof(string) }, null);
          var released = (InstallerRunner.PayloadMsiInfo)reader.Invoke(null, new object[] { args[2] });
          Check(released != null, "published 2.0.0 MSI opens as read-only metadata");
          Check(released.ProductCode == "{01A0F036-8DBA-7607-86C4-4534667DA302}" && released.VersionText == "2.0.99.0", "published 2.0.0 MSI identity matches extracted artifact");
          Check(released.SupportsTransactionalReplacement && released.UpgradeCode == "{E3FA501A-85F8-4187-85A7-D6E6BDC7EDA1}", "released 2.0.0 has the current transactional family contract");
          Check(InstallerRunner.CachedMsiPreservesDriversDuringUpgrade(args[2]), "released 2.0.0 driver removal is upgrade guarded");
          var releasedRoot = InstallerRunner.ReadCachedMigrationRoot(args[2], released.ProductCode, (product, component) => {
            Check(component == "{959189D7-EE12-55E9-B6A8-F5A8F7BB7D22}", "released executable component read from actual MSI");
            return Path.Combine(source, "sunshine.exe");
          });
          Check(releasedRoot == source, "released MSI component query resolves reported layout");
          Check(InstallerRunner.CanReplaceInMsiTransaction(Product(true, source, released.ProductCode), released,
            Metadata(Other, released.UpgradeCode)), "released 2.0.0 enters normal MSI replacement after bootstrap preparation");
        }
      }
      string checkpointPath;
      using (var checkpoint = InstallerRunner.LegacyDataCheckpoint.Open(recovery, source, target, Code, security)) {
        checkpointPath = checkpoint.DirectoryPath;
        checkpoint.SeedDestination();
        Check(Read(target, "config/sunshine_state.json") == "paired-client-secret", "pairing preserved before install");
        Check(Read(target, "credentials/key.pem") == "private-key", "credentials preserved");
        Check(Read(target, "apps.json") == "flat-legacy-apps", "flat legacy layout preserved");
        Check(Read(target, "covers/game.png") == "image-bytes", "covers preserved");
        Check(Read(target, "config/vibeshine_state.json") == "display-and-pairing-settings", "reported 2.0.0 vibeshine state preserved");
        try { using (InstallerRunner.LegacyDataCheckpoint.Open(recovery, source, target, Code, security)) { } throw new Exception("concurrent source migration accepted"); }
        catch (IOException) { checks++; }
      }
      // No finish simulates process loss: ready checkpoint must be reused.
      using (var checkpoint = InstallerRunner.LegacyDataCheckpoint.Open(recovery, source, target, Code, security)) {
        Check(checkpoint.DirectoryPath == checkpointPath, "interruption reuses durable checkpoint");
        checkpoint.SeedDestination();
        Write(source, "config/sunshine.conf", "MSI mutated config"); checkpoint.RestoreSource();
        Check(!Directory.Exists(Path.Combine(target, "config")), "failed relocation restores original destination absence");
        Check(Read(source, "config/sunshine.conf") == "unicode = 日本\n", "failed/canceled MSI data restored byte for byte");
      }
      // Changes made by the user after rollback become the next retry baseline.
      Write(source, "config/sunshine.conf", "user edited after rollback");
      using (var checkpoint = InstallerRunner.LegacyDataCheckpoint.Open(recovery, source, target, Code, security)) {
        Write(target, "config/sunshine.conf", "destination conflict");
        Reject(() => checkpoint.SeedDestination(), "conflicting target data must fail before overwrite");
        Check(Read(target, "config/sunshine.conf") == "destination conflict", "collision preserves destination");
        Write(source, "config/sunshine.conf", "second failed MSI"); checkpoint.RestoreSource();
        Check(Read(source, "config/sunshine.conf") == "user edited after rollback", "retry refreshes restored baseline");
      }
      using (var checkpoint = InstallerRunner.LegacyDataCheckpoint.Open(recovery, source, source, Other, security)) {
        checkpoint.SeedDestination(); checkpoint.Complete(); Write(source, "config/sunshine.conf", "post-upgrade setting");
        checkpoint.RestoreSource(); Check(Read(source, "config/sunshine.conf") == "post-upgrade setting", "completed checkpoint never rolls back committed install");
      }
      using (var checkpoint = InstallerRunner.LegacyDataCheckpoint.Open(recovery, source, source, Other, security)) {
        checkpoint.RestoreSource(); Check(Read(source, "config/sunshine.conf") == "post-upgrade setting", "completed retry idempotent");
      }
      using (var checkpoint = InstallerRunner.LegacyDataCheckpoint.Open(recovery, source, source, Other, security, services => { }, new string[0])) {
        Write(source, "config/sunshine.conf", "failed later maintenance"); checkpoint.RestoreSource();
        Check(Read(source, "config/sunshine.conf") == "post-upgrade setting", "new maintenance attempt checkpoints current settings after completed migration");
      }
      Reject(() => InstallerRunner.LegacyDataCheckpoint.Open(recovery, source, Path.Combine(root, "different target"), Code, security), "interrupted journal cannot change destination");
      var flat = Path.Combine(root, "flat layout"); Directory.CreateDirectory(flat); Write(flat, "apps.json", "original-apps");
      using (var checkpoint = InstallerRunner.LegacyDataCheckpoint.Open(recovery, flat, flat, Code, security)) {
        InstallerRunner.LegacyDataCheckpoint.PrepareConfigurationLayout(flat, "0");
        Check(Read(flat, "config/apps.json") == "original-apps", "bootstrap moves flat data into installed layout before MSI");
        Check(Read(flat, "config/sunshine.conf").Contains("dd_use_sunshine_virtual_display_driver = disabled"), "bootstrap applies selected backend before MSI");
        Check(Read(flat, "apps.json") == "original-apps", "flat original retained until install succeeds");
        InstallerRunner.LegacyDataCheckpoint.PrepareConfigurationLayout(flat, "0");
        Check(Read(flat, "config/sunshine.conf").Split(new[] { "dd_use_sunshine_virtual_display_driver" }, StringSplitOptions.None).Length == 2, "layout/preference preparation is idempotent");
        Write(flat, "config/apps.json", "failed MSI moved app data"); checkpoint.RestoreSource();
        Check(!Directory.Exists(Path.Combine(flat, "config")) && Read(flat, "apps.json") == "original-apps", "rollback restores absence of config directory for flat layouts");
        Check(Directory.GetDirectories(checkpoint.DirectoryPath, "failed-data-*").Length == 1, "failed-attempt data retained rather than deleted");
      }
      var corrupt = Path.Combine(root, "corrupt snapshot"); Directory.CreateDirectory(corrupt); Write(corrupt, "apps.json", "preserve");
      string corruptPath;
      using (var checkpoint = InstallerRunner.LegacyDataCheckpoint.Open(recovery, corrupt, corrupt, Code, security)) { corruptPath = checkpoint.DirectoryPath; }
      var snapshot = Directory.GetDirectories(corruptPath, "source-*").Single(path => !path.EndsWith("-destination")); Write(snapshot, "apps.json", "tampered");
      Reject(() => InstallerRunner.LegacyDataCheckpoint.Open(recovery, corrupt, corrupt, Code, security), "corrupt snapshot must not be restored");
      Check(Read(corrupt, "apps.json") == "preserve", "integrity failure leaves source unchanged");
      var serviceSource = Path.Combine(root, "service state"); Directory.CreateDirectory(serviceSource); Write(serviceSource, "apps.json", "service-data");
      using (var checkpoint = InstallerRunner.LegacyDataCheckpoint.Open(recovery, serviceSource, serviceSource, Code, security,
          services => Check(services.SequenceEqual(new[] { "FixtureService" }), "running services persisted before mocked quiesce"), new[] { "FixtureService" })) { }
      using (var checkpoint = InstallerRunner.LegacyDataCheckpoint.Open(recovery, serviceSource, serviceSource, Code, security,
          services => Check(services.SequenceEqual(new[] { "FixtureService" }), "interrupted stopped service retains original running state"), new string[0])) { }
      Console.WriteLine("Production migration resolver and real filesystem checkpoint: " + checks + " checks passed. MSI/registry/service/driver operations were not executed.");
    } finally { Directory.Delete(root, true); }
  }
}
