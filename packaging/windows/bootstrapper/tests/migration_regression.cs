using System;
using System.IO;
using System.Linq;
using System.Reflection;
using PRODUCT_NAMESPACE;

internal static class MigrationRegression {
  private static int checks;
  private const string Code = "{11111111-1111-1111-1111-111111111111}";
  private const string Other = "{22222222-2222-2222-2222-222222222222}";
  private static void Check(bool condition, string name) { if (!condition) throw new Exception(name); checks++; }
  private static object Call(string name, params object[] args) {
    return typeof(InstallerRunner).GetMethod(name, BindingFlags.Static | BindingFlags.NonPublic).Invoke(null, args);
  }
  private static InstallerRunner.InstalledProductInfo Product(bool msi, string root, string code = Code,
      InstallerRunner.InstalledProductKind kind = InstallerRunner.InstalledProductKind.Vibepollo, bool perUser = false) {
    return new InstallerRunner.InstalledProductInfo { IsWindowsInstaller = msi, IsPerUser = perUser,
      Kind = kind, ProductCode = msi ? code : "", InstallLocation = root,
      RegistryPath = "HKLM\\ARP\\" + (msi ? code : kind.ToString()),
      UninstallString = "\"" + Path.Combine(root, "uninstall.exe") + "\" /S", Version = new Version(2, 0, 99, 0) };
  }
  public static void Main(string[] args) {
    var root = Path.Combine(Path.GetTempPath(), "installed root ' & 日本");
    var otherRoot = root + " other";
    var facade = Product(false, root + Path.DirectorySeparatorChar);
    var msi = Product(true, root);
    var plan = InstallerRunner.ResolveInstalledProductFacades(new[] { facade, msi }, code => code == Code);
    Check(plan.Count == 1 && plan[0].IsWindowsInstaller && plan[0].ProductCode == Code, "2.0.0 facade resolves to hidden live MSI once");
    plan = InstallerRunner.ResolveInstalledProductFacades(new[] { facade, msi }, code => false);
    Check(plan.Count == 2 && plan.Any(p => !p.IsWindowsInstaller), "stale MSI cannot absorb executable uninstaller");
    plan = InstallerRunner.ResolveInstalledProductFacades(new[] { Product(false, root, perUser: true), msi }, code => true);
    Check(plan.Count == 2, "HKCU executable never becomes elevated machine MSI");
    plan = InstallerRunner.ResolveInstalledProductFacades(new[] { facade, Product(true, root, perUser: true) }, code => true);
    Check(plan.Count == 2, "per-user MSI cannot back machine facade");
    plan = InstallerRunner.ResolveInstalledProductFacades(new[] { facade, Product(true, otherRoot) }, code => true);
    Check(plan.Count == 2, "different installation root does not absorb facade");
    plan = InstallerRunner.ResolveInstalledProductFacades(new[] { facade, Product(true, root, kind: InstallerRunner.InstalledProductKind.Vibeshine) }, code => true);
    Check(plan.Count == 2, "cross-product identity remains a competitor");
    foreach (var kind in new[] { InstallerRunner.InstalledProductKind.Apollo, InstallerRunner.InstalledProductKind.Sunshine }) {
      plan = InstallerRunner.ResolveInstalledProductFacades(new[] { Product(false, root, kind: kind), Product(true, root, kind: kind) }, code => true);
      Check(plan.Count == 2 && plan.Any(p => !p.IsWindowsInstaller), "external legacy uninstaller remains available: " + kind);
    }
    try {
      InstallerRunner.ResolveInstalledProductFacades(new[] { facade, msi, Product(true, root, Other) }, code => true);
      throw new Exception("ambiguous MSI identity was accepted");
    } catch (InvalidOperationException) { checks++; }
    Check(InstallerRunner.ResolveInstalledProductFacades(new InstallerRunner.InstalledProductInfo[0], code => true).Count == 0, "fresh install has no migration prerequisite");
    if (args.Length > 0) {
      var queried = false;
      var found = InstallerRunner.ReadCachedMsiInstallRoot(args[0], Code, (product, component) => {
        Check(product == Code && component == Other, "cached MSI query selects sunshine.exe component identity");
        queried = true;
        return Path.Combine(root, "sunshine.exe");
      });
      Check(queried && found == root, "real metadata database resolves install root rather than ARP icon cache");
      Check(InstallerRunner.ReadCachedMsiInstallRoot(args[0], Code, (product, component) => null) == null, "unregistered component cannot invent root");
      Check(InstallerRunner.ReadCachedMsiInstallRoot(args[0], Code, (product, component) => Path.Combine(root, "unrelated.exe")) == null, "unrelated component key path cannot identify host root");
      Check(InstallerRunner.ReadCachedMsiInstallRoot("missing.msi", Code, (product, component) => root) == null, "missing cache returns no verified root");
      var payload = (InstallerRunner.PayloadMsiInfo)Call("TryGetPayloadMsiInfo", args[0]);
      var ownKind = "PRODUCT_NAMESPACE" == "VibepolloInstaller" ? InstallerRunner.InstalledProductKind.Vibepollo : InstallerRunner.InstalledProductKind.Vibeshine;
      var source = Product(true, root, kind: ownKind);
      Check(!(bool)Call("RequiresPreUninstallDowngradeWorkaround", source, args[0]), "2.0.0 to 2.0.1 daily keeps MSI rollback transaction");
      source.Version = new Version(9, 0, 99, 0);
      Check(!(bool)Call("RequiresPreUninstallDowngradeWorkaround", source, args[0]), "transactional downgrade remains protected");
      if (args.Length > 1 && !string.IsNullOrWhiteSpace(args[1])) {
        var released = (InstallerRunner.PayloadMsiInfo)Call("TryGetPayloadMsiInfo", args[1]);
        Check(released.ProductCode == "{01A0F036-8DBA-7607-86C4-4534667DA302}" && released.VersionText == "2.0.99.0", "published 2.0.0 identity is verified read-only");
        Check(released.SupportsTransactionalReplacement, "released 2.0.0 supports MSI replacement");
        found = InstallerRunner.ReadCachedMsiInstallRoot(args[1], released.ProductCode, (product, component) => {
          Check(component == "{959189D7-EE12-55E9-B6A8-F5A8F7BB7D22}", "actual 2.0.0 executable component is selected");
          return Path.Combine(root, "sunshine.exe");
        });
        Check(found == root, "published 2.0.0 resolves its actual installation path");
      }
    }
    Console.WriteLine(checks + " production facade, root, and transactional replacement checks passed.");
  }
}
