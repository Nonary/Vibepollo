using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using PRODUCT_NAMESPACE;

internal static class InstallSuccessRegression {
  static readonly BindingFlags PrivateStatic = BindingFlags.NonPublic | BindingFlags.Static;
  static readonly BindingFlags PrivateInstance = BindingFlags.NonPublic | BindingFlags.Instance;
  static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
  static T Field<T>(InstallerWindow window, string name) {
    return (T)typeof(InstallerWindow).GetField(name, PrivateInstance).GetValue(window);
  }
  static List<string> Failures(string path) {
    return (List<string>)typeof(InstallerRunner).GetMethod("CollectInstallComponentFailures", PrivateStatic)
      .Invoke(null, new object[] { path, true });
  }
  static bool Reboot(string path) {
    return (bool)typeof(InstallerRunner).GetMethod("InstallLogIndicatesDriverRebootRequired", PrivateStatic)
      .Invoke(null, new object[] { path });
  }
  static void Finish(string directory, string name, int exitCode, string log, string title, bool warning, bool reboot) {
    var result = new InstallerResult {
      Operation = InstallerOperation.Install, ExitCode = exitCode, LogPath = log,
      Message = "Fixture MSI result", ComponentFailures = Failures(log)
    };
    Check(result.Succeeded == (exitCode == 0 || exitCode == 3010), name + ": exit classification");
    Check(result.PartiallySucceeded == (result.Succeeded && warning), name + ": partial success");
    var window = new InstallerWindow(new InstallerArguments());
    Func<Task<InstallerResult>> action = () => Task.FromResult(result);
    var operation = (Task)typeof(InstallerWindow).GetMethod("RunOperationAsync", PrivateInstance)
      .Invoke(window, new object[] { action, "Install", "Installing fixture...", typeof(InstallerWindow).Namespace.Replace("Installer", "") + " installation completed." });
    Check(Field<TextBlock>(window, "_overlayTitleText").Text == title, name + ": final dialog title");
    var message = Field<TextBlock>(window, "_overlayMessageText").Text;
    Check(message.Contains("Save logs") == (warning || !result.Succeeded), name + ": support report prompt");
    if (result.Succeeded && !warning) {
      Check(Field<TextBlock>(window, "_statusText").Foreground == Field<Brush>(window, "_statusSuccessBrush"), name + ": green success");
      Check(Field<TextBlock>(window, "_statusDetailText").Text.Contains("restart is required") == reboot, name + ": reboot detail");
      Check(!message.Contains("failed") && !message.Contains("warnings"), name + ": no alarming message");
      Check(Field<Button>(window, "_overlaySecondaryButton").Visibility == Visibility.Collapsed, name + ": no report action");
    }
    // Render the actual production completion controls without showing a window.
    var root = (Grid)window.Content;
    window.Content = null;
    root.Measure(new Size(720, 700)); root.Arrange(new Rect(0, 0, 720, 700)); root.UpdateLayout();
    var bitmap = new RenderTargetBitmap(720, 700, 96, 96, PixelFormats.Pbgra32);
    bitmap.Render(root);
    var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
    using (var stream = File.Create(Path.Combine(directory, name + ".png"))) encoder.Save(stream);
    typeof(InstallerWindow).GetMethod("ResolveOverlay", PrivateInstance).Invoke(window, new object[] { "secondary" });
    operation.GetAwaiter().GetResult();
    Check(window.ProcessExitCode == (result.Succeeded ? 0 : exitCode), name + ": UI exit code");
    window.Close();
    Console.WriteLine(name + ": " + title + ", exit " + exitCode + ", " + result.ComponentFailures.Count + " component diagnostics");
  }
  [STAThread]
  static int Main(string[] args) {
    var directory = args[0];
    var diagnostic = Path.Combine(directory, "shared-unavailable.log");
    Check(File.ReadAllText(diagnostic).Contains("USBIP_TRANSPORT_INFO"), "Readiness detail remains in the MSI log");
    var saved = (string)typeof(InstallerRunner).GetMethod("PersistInstallLog", PrivateStatic)
      .Invoke(null, new object[] { diagnostic, Path.Combine(directory, "log-copy-fixture"), "install" });
    Check(File.ReadAllText(saved) == File.ReadAllText(diagnostic), "Production saved log preserves the informational diagnostic verbatim");
    foreach (var name in new[] { "shared-unavailable", "shared-ready", "new-ready" }) {
      var path = Path.Combine(directory, name + ".log");
      Check(Failures(path).Count == 0 && !Reboot(path), name + ": clean success classification");
      Finish(directory, name, 0, path, "Complete", false, false);
    }
    foreach (var name in new[] { "new-not-ready", "new-reboot" }) {
      var path = Path.Combine(directory, name + ".log");
      Check(Failures(path).Count == 0 && Reboot(path), name + ": reboot remains success");
      Finish(directory, name, 3010, path, "Complete", false, true);
    }
    var failure = Path.Combine(directory, "staging-failed.log");
    Check(Failures(failure).Count > 0, "USB/IP staging error must remain visible");
    Finish(directory, "optional-failure", 0, failure, "Install completed with warnings", true, false);
    Finish(directory, "msi-failure", 1603, Path.Combine(directory, "new-ready.log"), "Install failed", false, false);
    foreach (var marker in new[] { "VIRTUAL_DISPLAY_DRIVER_WARNING", "VIRTUAL_GAMEPAD_DRIVER_WARNING", "CustomAction InstallUsbIpTransport returned actual error code 1603" }) {
      var path = Path.Combine(directory, "required-failure.log");
      File.WriteAllText(path, marker);
      Check(Failures(path).Count > 0, marker + ": genuine component error retained");
    }
    Console.WriteLine("Production log classification and completion UI regressions passed; no installer, service, driver or trust actions executed.");
    return 0;
  }
}
