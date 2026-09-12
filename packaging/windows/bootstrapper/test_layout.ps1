#Requires -Version 5.1
# Compile and measure the real WPF layout without showing a window or installing anything.
$ErrorActionPreference = 'Stop'
$testDir = Join-Path ([IO.Path]::GetTempPath()) ('vibeshine-layout-' + [Guid]::NewGuid())
[void][IO.Directory]::CreateDirectory($testDir)
$harness = Join-Path $testDir 'LayoutTest.cs'
@'
using System;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using VibepolloInstaller;

internal static class LayoutTest {
  static T Field<T>(InstallerWindow window, string name) {
    return (T)typeof(InstallerWindow).GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).GetValue(window);
  }
  static T Find<T>(DependencyObject parent) where T : DependencyObject {
    if (parent is T) return (T)parent;
    for (int i = 0; i < VisualTreeHelper.GetChildrenCount(parent); ++i) {
      var found = Find<T>(VisualTreeHelper.GetChild(parent, i));
      if (found != null) return found;
    }
    return null;
  }
  [STAThread]
  static int Main() {
    var window = new InstallerWindow(new InstallerArguments());
    var root = (Grid)window.Content;
    // Measure independently of the current desktop's resolution and DPI.
    window.Content = null;
    root.Measure(new Size(720, 640));
    root.Arrange(new Rect(0, 0, 720, 640));
    root.UpdateLayout();
    var scroll = Find<ScrollViewer>(root);
    int checks = 0;
    foreach (bool options in new[] { false, true }) {
      foreach (string name in new[] { "_installSection", "_installVirtualDisplaySection", "_installVirtualGamepadSection" })
        Field<Border>(window, name).Visibility = options ? Visibility.Visible : Visibility.Collapsed;
      foreach (bool expanded in new[] { false, true }) {
        Find<Expander>(root).IsExpanded = expanded;
        foreach (bool busy in new[] { false, true }) {
          Field<ProgressBar>(window, "_progressBar").Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
          foreach (var size in new[] { new Size(720, 700), new Size(720, 640), new Size(640, 360), new Size(480, 320) }) {
            foreach (string name in new[] { "_continueButton", "_uninstallButton", "_licenseButton", "_closeButton" })
              Field<Button>(window, name).Visibility = Visibility.Visible;
            root.Measure(size);
            root.Arrange(new Rect(size));
            root.UpdateLayout();
            foreach (string name in new[] { "_continueButton", "_uninstallButton", "_licenseButton", "_closeButton" }) {
              var button = Field<Button>(window, name);
              for (var parent = VisualTreeHelper.GetParent(button); parent != null; parent = VisualTreeHelper.GetParent(parent)) {
                var element = parent as FrameworkElement;
                if (element == null) continue;
                var bounds = button.TransformToAncestor(element).TransformBounds(new Rect(button.RenderSize));
                if (button.ActualHeight < 40 || bounds.Left < -0.1 || bounds.Top < -0.1 || bounds.Right > element.ActualWidth + 0.1 || bounds.Bottom > element.ActualHeight + 0.1)
                  throw new Exception(name + " clipped at " + size + " by " + element.GetType().Name);
              }
            }
            if (options && expanded && size.Height == 320 && scroll.ScrollableHeight <= 0)
              throw new Exception("Overflowing options must be scrollable.");
            scroll.ScrollToEnd();
            root.UpdateLayout();
            if (Math.Abs(scroll.VerticalOffset - scroll.ScrollableHeight) > 0.1)
              throw new Exception("Cannot reach the bottom of the options.");
            ++checks;
          }
        }
      }
    }
    Console.WriteLine("Passed " + checks + " WPF layout cases: actions fully visible and content reachable.");
    return 0;
  }
}
'@ | Set-Content -LiteralPath $harness -Encoding UTF8
$framework = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319'
$compilerArgs = @('/nologo', '/target:exe', '/main:LayoutTest', "/out:$testDir/LayoutTest.exe")
foreach ($reference in @('System', 'System.Core', 'System.Data', 'System.Xml', 'System.Xaml', 'System.Windows.Forms')) {
    $compilerArgs += "/reference:$framework/$reference.dll"
}
foreach ($reference in @('WindowsBase', 'PresentationCore', 'PresentationFramework')) {
    $compilerArgs += "/reference:$framework/WPF/$reference.dll"
}
$compilerArgs += (Join-Path $PSScriptRoot 'VibeshineInstaller.cs')
$compilerArgs += $harness
& "$framework/csc.exe" @compilerArgs
if ($LASTEXITCODE -ne 0) { throw 'Layout test compilation failed.' }
& "$testDir/LayoutTest.exe"
if ($LASTEXITCODE -ne 0) { throw 'Layout test failed.' }
