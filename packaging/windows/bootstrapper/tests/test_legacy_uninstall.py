#!/usr/bin/env python3
"""Exercise the real NSIS detection/launch methods without Windows or the WPF UI.

Usage: python test_legacy_uninstall.py [path/to/dotnet] (requires .NET 8 SDK)
The process boundary is stubbed; a Windows migration remains an integration test.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

source = (pathlib.Path(__file__).resolve().parents[1] / "VibeshineInstaller.cs").read_text()
methods = []
for name in ("RunUninstallCommand", "IsNsisUninstaller", "TrySplitExecutableAndArguments"):
    match = re.search(r"^    private static [^\n]+ " + name + r"\([^\n]*\) \{\n.*?^    \}", source, re.M | re.S)
    if match is None:
        raise RuntimeError("Cannot locate method " + name)
    methods.append(match.group())

harness = r'''
using System;
using System.IO;
class Test {
  class InstalledProductInfo {
    public bool IsWindowsInstaller;
    public string UninstallString = "";
  }
  static string BuildSilentUninstallCommand(InstalledProductInfo p) => p == null ? "" : p.UninstallString;
  static void TryDeleteFile(string path) => File.Delete(path);
  static string launchedPath = "", launchedArguments = "";
  static bool launchThrows;
  static int RunProcess(string path, string arguments, bool hidden, bool elevate) {
    Check(File.Exists(path), "launched executable exists");
    launchedPath = path;
    launchedArguments = arguments;
    if (launchThrows) throw new InvalidOperationException("simulated start failure");
    return 3010;
  }
  static void Check(bool condition, string name) {
    if (!condition) throw new Exception(name);
  }
  static void Fixture(string path, uint flags = 1, uint signature = 0xDEADBEEF, int offset = 512) {
    using (var writer = new BinaryWriter(File.Create(path))) {
      writer.Write(new byte[offset]);
      foreach (uint word in new uint[] { flags, signature, 0x6C6C754E, 0x74666F73, 0x74736E49, 0, 28 })
        writer.Write(word);
    }
  }
  static void Main() {
    var root = Path.Combine(Path.GetTempPath(), "NSIS custom path " + Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try {
      var exe = Path.Combine(root, "Uninstall.exe");
      Fixture(exe);
      Check(IsNsisUninstaller(exe), "recognize NSIS uninstaller");
      Fixture(exe, flags: 3, offset: 1024);
      Check(IsNsisUninstaller(exe), "recognize silent uninstaller at later offset");
      Fixture(exe, flags: 17);
      Check(!IsNsisUninstaller(exe), "reject unknown NSIS header flags");
      Check(!IsNsisUninstaller(Path.Combine(root, "missing.exe")), "missing executable falls back to normal process launch");
      Check(!IsNsisUninstaller("missing-path-command.exe"), "PATH command detection falls back safely");
      Check(!IsNsisUninstaller(root), "unreadable executable falls back safely");
      Check(RunUninstallCommand(null, true, false) == 1, "preserve null product handling");
      Fixture(exe, flags: 0);
      Check(!IsNsisUninstaller(exe), "do not treat NSIS installer as uninstaller");
      Fixture(exe, signature: 0);
      Check(!IsNsisUninstaller(exe), "reject unrelated executable");
      Fixture(exe, offset: 513);
      Check(!IsNsisUninstaller(exe), "reject unaligned signature");
      File.WriteAllBytes(exe, new byte[530]);
      Check(!IsNsisUninstaller(exe), "reject truncated header without overread");
      Fixture(exe);
      var product = new InstalledProductInfo { UninstallString = "\"" + exe + "\" /S" };
      var code = RunUninstallCommand(product, true, false);
      Check(code == 3010, "preserve uninstaller exit code");
      Check(launchedPath != exe, "execute copy so original can be deleted");
      Check(launchedArguments == "/S _?=" + root, "append unquoted custom path last");
      Check(!File.Exists(launchedPath) && !Directory.Exists(Path.GetDirectoryName(launchedPath)), "clean temporary copy after exit");
      Check(File.Exists(exe), "launcher must not delete original payload itself");
      launchThrows = true;
      try { RunUninstallCommand(product, true, false); throw new Exception("expected launch failure"); }
      catch (InvalidOperationException) { }
      Check(!Directory.Exists(Path.GetDirectoryName(launchedPath)), "clean copy after launch failure");
      launchThrows = false;
      product.UninstallString += " _?=" + root;
      RunUninstallCommand(product, true, false);
      Check(launchedPath == exe && launchedArguments == "/S _?=" + root, "preserve existing synchronous command");
      product.UninstallString = "\"" + exe + "\" /S";
      Fixture(exe, flags: 0);
      RunUninstallCommand(product, true, false);
      Check(launchedPath == exe && launchedArguments == "/S", "preserve other executable commands");
      Fixture(exe);
      product.IsWindowsInstaller = true;
      RunUninstallCommand(product, true, false);
      Check(launchedPath == exe, "do not change MSI commands");
      Console.WriteLine("NSIS detection, synchronous launch, custom paths, cleanup, and fallback checks passed.");
    } finally { Directory.Delete(root, true); }
  }
'''
with tempfile.TemporaryDirectory(prefix="vibeshine-nsis-test-") as directory:
    path = pathlib.Path(directory)
    (path / "Test.csproj").write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework></PropertyGroup></Project>')
    (path / "NuGet.Config").write_text('<configuration><packageSources><clear /></packageSources></configuration>')
    (path / "Test.cs").write_text(harness + "\n".join(methods) + "\n}")
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else "dotnet", "run", "--project", str(path / "Test.csproj")], check=True)
