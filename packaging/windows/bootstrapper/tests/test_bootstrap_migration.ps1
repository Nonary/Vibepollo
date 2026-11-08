param([Parameter(Mandatory=$true)][string]$OutputDirectory, [string]$Published200MsiPath = '')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$source = Join-Path $repo 'packaging/windows/bootstrapper/VibeshineInstaller.cs'
$sourceText = Get-Content -LiteralPath $source -Raw -Encoding UTF8
$namespace = if ($sourceText.Contains('namespace VibepolloInstaller')) { 'VibepolloInstaller' } else { 'VibeshineInstaller' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$harness = Join-Path $OutputDirectory 'migration-regression.cs'
[IO.File]::WriteAllText($harness, (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'migration_regression.cs') -Raw -Encoding UTF8).Replace('PRODUCT_NAMESPACE', $namespace), [Text.UTF8Encoding]::new($false))
$framework = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319'
$output = Join-Path $OutputDirectory 'migration-regression.exe'
$references = @()
foreach ($reference in @('System.dll','System.Core.dll','System.Data.dll','System.Xml.dll','System.Xaml.dll','System.Windows.Forms.dll','WPF/WindowsBase.dll','WPF/PresentationCore.dll','WPF/PresentationFramework.dll')) { $references += '/reference:' + (Join-Path $framework $reference) }
& (Join-Path $framework 'csc.exe') /nologo /target:exe /main:MigrationRegression "/out:$output" @references $source $harness
if ($LASTEXITCODE -ne 0) { throw 'Production installer compilation failed' }
$wix = Join-Path ${env:ProgramFiles(x86)} 'WiX Toolset v3.14/bin'
if (!(Test-Path -LiteralPath (Join-Path $wix 'candle.exe'))) { throw 'WiX required for real MSI metadata regression' }
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'payload.txt'), 'Metadata fixture only; never install.')
$fixtureSource = @'
<?xml version="1.0" encoding="utf-8"?>
<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi">
  <Product Id="{11111111-1111-1111-1111-111111111111}" Name="Metadata fixture - never install" Language="1033" Version="2.0.100" Manufacturer="Test" UpgradeCode="{33333333-3333-3333-3333-333333333333}">
    <Package InstallerVersion="500" Compressed="yes" InstallScope="perMachine"/>
    <MediaTemplate EmbedCab="yes"/>
    <Property Id="VIBESHINE_TRANSACTIONAL_REPLACEMENT" Value="1"/>
    <Property Id="VIBEPOLLO_TRANSACTIONAL_REPLACEMENT" Value="1"/>
    <Directory Id="TARGETDIR" Name="SourceDir"><Directory Id="ProgramFilesFolder"><Directory Id="INSTALL_ROOT" Name="FixtureNeverInstall">
      <Component Id="HostExecutable" Guid="{22222222-2222-2222-2222-222222222222}"><File Id="Host" Name="sunshine.exe" Source="$(var.FixtureRoot)/payload.txt" KeyPath="yes"/></Component>
    </Directory></Directory></Directory>
    <Feature Id="Application" Level="1"><ComponentRef Id="HostExecutable"/></Feature>
  </Product>
</Wix>
'@
$fixtureWxs = Join-Path $OutputDirectory 'metadata-fixture.wxs'
[IO.File]::WriteAllText($fixtureWxs, $fixtureSource)
& (Join-Path $wix 'candle.exe') -nologo "-dFixtureRoot=$OutputDirectory" -out (Join-Path $OutputDirectory 'metadata-fixture.wixobj') $fixtureWxs
if ($LASTEXITCODE -ne 0) { throw 'Metadata fixture WiX compile failed' }
$fixtureMsi = Join-Path $OutputDirectory 'metadata-fixture.msi'
& (Join-Path $wix 'light.exe') -nologo -sval -out $fixtureMsi (Join-Path $OutputDirectory 'metadata-fixture.wixobj')
if ($LASTEXITCODE -ne 0) { throw 'Metadata fixture WiX link failed' }
if ($Published200MsiPath) { $Published200MsiPath = [IO.Path]::GetFullPath($Published200MsiPath) }
& $output $fixtureMsi $Published200MsiPath
if ($LASTEXITCODE -ne 0) { throw 'Installer upgrade regression failed' }

# Compile an isolated copy of production source with only registry enumeration
# and external uninstaller launch stubbed. Execute the real competing-products
# dispatcher; no MSI install, process/service stop, or registry write occurs.
function Replace-TestMethod([string]$text, [string]$name, [string]$replacement) {
    $pattern = '(?ms)^    (?:private|public) static [^\r\n]+\b' + $name + '\(.*?^    \}\r?\n'
    $matches = [regex]::Matches($text, $pattern)
    if ($matches.Count -ne 1) { throw "Cannot locate production method $name" }
    return $text.Remove($matches[0].Index, $matches[0].Length).Insert($matches[0].Index, $replacement + "`n")
}
$dispatchSource = Replace-TestMethod $sourceText 'GetInstalledProductRegistrations' @'
    private static List<InstalledProductInfo> GetInstalledProductRegistrations(bool includeSunshine) {
      return LegacyDispatch.Products;
    }
'@
$dispatchSource = Replace-TestMethod $dispatchSource 'RunUninstallCommand' @'
    private static int RunUninstallCommand(InstalledProductInfo product, bool hiddenWindow, bool requestElevationIfNeeded) {
      LegacyDispatch.Calls++;
      LegacyDispatch.Elevated = requestElevationIfNeeded;
      return LegacyDispatch.ExitCode;
    }
'@
$dispatchSource = Replace-TestMethod $dispatchSource 'GetInstalledVibepolloProduct' @'
    public static InstalledProductInfo GetInstalledVibepolloProduct() {
      return LegacyDispatch.OwnProduct;
    }
'@
$dispatchPath = Join-Path $OutputDirectory 'dispatch-production.cs'
[IO.File]::WriteAllText($dispatchPath, $dispatchSource, [Text.UTF8Encoding]::new($false))
$dispatchHarness = @'
using System;
using System.Collections.Generic;
using System.Reflection;
using PRODUCT_NAMESPACE;
internal static class LegacyDispatch {
  internal static List<InstallerRunner.InstalledProductInfo> Products;
  internal static InstallerRunner.InstalledProductInfo OwnProduct;
  internal static int Calls, ExitCode;
  internal static bool Elevated;
  static void Check(bool value, string name) { if (!value) throw new Exception(name); }
  static void Main() {
    var method = typeof(InstallerRunner).GetMethod("UninstallCompetingProducts", BindingFlags.Static | BindingFlags.NonPublic);
    foreach (var kind in new[] {InstallerRunner.InstalledProductKind.Apollo, InstallerRunner.InstalledProductKind.Sunshine}) {
      foreach (var perUser in new[] {false, true}) {
        foreach (var phase in new[] {"install_remove_competing", "cli_remove_competing"}) {
          foreach (var code in new[] {0, 3010, 1603}) {
            Products = new List<InstallerRunner.InstalledProductInfo> { new InstallerRunner.InstalledProductInfo {
              Kind = kind, IsPerUser = perUser, IsWindowsInstaller = false, RegistryPath = kind.ToString(), UninstallString = "legacy.exe /S" } };
            Calls = 0; ExitCode = code; Elevated = false;
            var result = (InstallerResult)method.Invoke(null, new object[] { phase, true, true });
            Check(Calls == 1, kind + " must reach its uninstaller during " + phase);
            Check(result.ExitCode == code, "preserve success, reboot, and failure outcomes");
            Check(Elevated == !perUser, "preserve HKCU command elevation restriction");
          }
        }
      }
    }
    Console.WriteLine("24 production Sunshine/Apollo uninstall dispatch scenarios passed (launch boundary stubbed).");
    var root = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "original 2.0.0 host");
    OwnProduct = new InstallerRunner.InstalledProductInfo { InstallLocation = root };
    var preserve = typeof(InstallerRunner).GetMethod("PreserveCliInstallDirectory", BindingFlags.Static | BindingFlags.NonPublic);
    var property = typeof(InstallerRunner).GetMethod("GetPropertyValue", BindingFlags.Static | BindingFlags.NonPublic);
    foreach (var operation in new[] {"/i", "/package"}) {
      var forwarded = new List<string> { operation, "payload.msi" };
      preserve.Invoke(null, new object[] {forwarded});
      Check((string)property.Invoke(null, new object[] {forwarded, "INSTALL_ROOT"}) == root, "quiet upgrade preserves 2.0.0 root");
      forwarded = new List<string> {operation, "payload.msi", "INSTALL_ROOT=explicit"};
      preserve.Invoke(null, new object[] {forwarded});
      Check(forwarded.Count == 3 && forwarded[2] == "INSTALL_ROOT=explicit", "caller-selected install root survives");
    }
    foreach (var operation in new[] {"/x", "/f"}) {
      var forwarded = new List<string> {operation, "payload.msi"};
      preserve.Invoke(null, new object[] {forwarded});
      Check(forwarded.Count == 2, "uninstall and repair args remain unchanged");
    }
    OwnProduct = null;
    var fresh = new List<string> {"/i", "payload.msi"};
    preserve.Invoke(null, new object[] {fresh});
    Check(fresh.Count == 2, "fresh install retains its default directory");
    Console.WriteLine("7 production quiet-upgrade directory scenarios passed (inventory boundary stubbed).");
  }
}
'@
$dispatchHarnessPath = Join-Path $OutputDirectory 'dispatch-regression.cs'
[IO.File]::WriteAllText($dispatchHarnessPath, $dispatchHarness.Replace('PRODUCT_NAMESPACE', $namespace), [Text.UTF8Encoding]::new($false))
$dispatchOutput = Join-Path $OutputDirectory 'dispatch-regression.exe'
& (Join-Path $framework 'csc.exe') /nologo /target:exe /main:LegacyDispatch "/out:$dispatchOutput" @references $dispatchPath $dispatchHarnessPath
if ($LASTEXITCODE -ne 0) { throw 'Production legacy dispatcher compilation failed' }
& $dispatchOutput
if ($LASTEXITCODE -ne 0) { throw 'Legacy uninstall dispatcher regression failed' }

# Run the shared full-source rollback/transactional dispatch regression too.
& (Join-Path $PSScriptRoot 'test_bootstrap_rollback.ps1') -OutputDirectory (Join-Path $OutputDirectory 'rollback')

[xml]$patch = Get-Content -LiteralPath (Join-Path $repo 'packaging/windows/wix/patch_custom_actions.wxs') -Raw
[xml]$custom = Get-Content -LiteralPath (Join-Path $repo 'packaging/windows/wix/custom_actions.wxs') -Raw
$ns = New-Object Xml.XmlNamespaceManager($custom.NameTable)
$ns.AddNamespace('w', 'http://schemas.microsoft.com/wix/2006/wi')
foreach ($action in @('AskUninstallLegacySunshine','WaitForLegacyUninstall','BlockUserCancelledLegacy','BlockLegacySunshineStillPresent','MigrateConfig','SetMigrateConfig','RunInstallerMigrations','SetRunInstallerMigrations','RemoveConflictingProducts')) {
    if ($patch.SelectNodes("//Custom[@Action='$action']").Count -ne 1 -or $custom.SelectNodes("//w:CustomAction[@Id='$action']", $ns).Count -ne 1) { throw "Existing MSI action must remain authored and scheduled: $action" }
}
foreach ($script in @('ask_uninstall_legacy_sunshine.vbs','wait_for_legacy_uninstall.vbs','remove_conflicting_products.vbs')) {
    if (!(Test-Path -LiteralPath (Join-Path $repo "packaging/windows/wix/$script"))) { throw "Missing legacy script: $script" }
}
if ($sourceText.Contains('UpgradeSourcePreUninstallVersion') -or $sourceText.Contains('RunWithBootstrapMigration')) { throw 'Superseded broad migration/version workaround remains' }
if ($namespace -eq 'VibepolloInstaller') {
    if ($custom.SelectNodes('//w:Component[@Id="RemoveLegacyApolloUninstallKey"]', $ns).Count -ne 1 -or $patch.SelectNodes('//ComponentRef[@Id="RemoveLegacyApolloUninstallKey"]').Count -ne 1) { throw 'Existing Apollo ARP cleanup must remain authored and referenced' }
}
Write-Output 'Existing MSI uninstall/configuration actions and focused upgrade workaround checks passed.'
