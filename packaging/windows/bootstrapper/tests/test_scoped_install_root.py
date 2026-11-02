#!/usr/bin/env python3
"""Exercise the production managed root validator with Windows API boundaries modeled."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
source = (ROOT / 'packaging/windows/bootstrapper/VibeshineInstaller.cs').read_text()
method, = re.findall(r'^    internal static string ValidateInstallRootPath\(.*?^    \}', source, re.M | re.S)
code = r'''
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
namespace ScopeRegression {
static class Environment {
  public enum SpecialFolder { ProgramFiles, ProgramFilesX86 }
  public static string GetFolderPath(SpecialFolder folder) { return @"C:\Program Files"; }
  public static string GetEnvironmentVariable(string name) { return @"C:\Program Files"; }
}
static class Path {
  public const char DirectorySeparatorChar='\\', AltDirectorySeparatorChar='/';
  public static string GetFullPath(string path) { return path; }
  public static string GetPathRoot(string path) { return path.Length<3?path:path.Substring(0,3); }
  public static string GetDirectoryName(string path) { int i=path.LastIndexOf('\\');return i<3?null:path.Substring(0,i); }
}
sealed class DirectoryInfo {
  string path;
  public DirectoryInfo(string value) { path=value; }
  public void Refresh() { }
  public FileAttributes Attributes { get { return Directory.Nodes[path]; } }
}
static class File {
  public static FileAttributes GetAttributes(string path) { throw new Exception("Unrelated descendant inspected: "+path); }
}
static class Directory {
  public static Dictionary<string,FileAttributes> Nodes = new Dictionary<string,FileAttributes>(StringComparer.OrdinalIgnoreCase);
  public static bool Exists(string path) { return Nodes.ContainsKey(path) && (Nodes[path]&FileAttributes.Directory)!=0; }
  public static IEnumerable<string> EnumerateFileSystemEntries(string path) {
    string prefix=path+@"\";
    return Nodes.Keys.Where(key=>key.StartsWith(prefix,StringComparison.OrdinalIgnoreCase) && !key.Substring(prefix.Length).Contains(@"\")).ToArray();
  }
}
static class Program {
  static bool Registered=true;
  static string NormalizePath(string value) { return value.TrimEnd('\\','/'); }
  static bool PathIsUnderDirectory(string value,string root) { return value.StartsWith(root+@"\",StringComparison.OrdinalIgnoreCase); }
  static bool IsRegisteredInstallRoot(string value) { return Registered; }
METHOD
  static void Check(bool value,string why) { if(!value)throw new Exception(why); }
  static void Reject(string path) {
    try { ValidateInstallRootPath(path); } catch(InvalidOperationException) { return; }
    throw new Exception("Expected rejection: "+path);
  }
  static void Main() {
    const string root=@"C:\Program Files\Sunshine";
    Directory.Nodes[@"C:\Program Files"]=FileAttributes.Directory;
    Directory.Nodes[root]=FileAttributes.Directory;
    Directory.Nodes[root+@"\personal-artwork-link"]=FileAttributes.ReparsePoint|FileAttributes.Directory;
    Directory.Nodes[root+@"\custom.png"]=FileAttributes.ReparsePoint;
    Check(ValidateInstallRootPath(root)==root,"registered root with unrelated links remains eligible for native preflight");
    Directory.Nodes[root]|=FileAttributes.ReparsePoint;Reject(root);
    Directory.Nodes[root]=FileAttributes.Directory;
    Directory.Nodes[@"C:\Program Files"]|=FileAttributes.ReparsePoint;Reject(root);
    Directory.Nodes[@"C:\Program Files"]=FileAttributes.Directory;
    Registered=false;Reject(root);
    Directory.Nodes.Remove(root+@"\personal-artwork-link");Directory.Nodes.Remove(root+@"\custom.png");
    Check(ValidateInstallRootPath(root)==root,"new empty destination accepted");
    Reject(@"C:\");Reject(@"C:\Elsewhere\Sunshine");
    Console.WriteLine("Managed scope: unrelated links ignored; selected root, ancestor links, destination boundary and nonempty unregistered roots still rejected.");
  }
}
}
'''.replace('METHOD', method)
dotnet = sys.argv[1] if len(sys.argv) > 1 else 'dotnet'
major = subprocess.check_output([dotnet, '--version'], text=True).strip().split('.')[0]
with tempfile.TemporaryDirectory(prefix='scoped-install-root-') as directory:
    path = Path(directory)
    (path / 'Test.csproj').write_text(f'<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net{major}.0</TargetFramework></PropertyGroup></Project>')
    (path / 'NuGet.Config').write_text('<configuration><packageSources><clear /></packageSources></configuration>')
    (path / 'Test.cs').write_text(code)
    env = dict(os.environ, DOTNET_CLI_HOME=str(path / 'home'), DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1', DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1')
    subprocess.run([dotnet, 'run', '--project', str(path / 'Test.csproj')], check=True, env=env)
