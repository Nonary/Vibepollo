#!/usr/bin/env python3
"""Run production rollback methods and the Windows harness without WPF (.NET 8).

Usage: python test_bootstrap_rollback.py [path/to/dotnet]
Only the external boundaries in rollback_boundary_methods.cs.in are stubbed.
This does not replace the Windows full-source/MSI/live rollback integration tests.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
PRODUCTION_METHODS = (
    "TryStashInstalledProductPayload",
    "BuildRollbackPreservationFailure", "TryRestoreStashedVibeshinePayload",
    "ApplyStashedPayloadRecovery", "TryPreUninstallDowngradeSourceVersion",
    "RequiresPreUninstallDowngradeWorkaround", "PayloadSupportsTransactionalReplacement",
    "HasDifferentProductCode", "NormalizeProductCode", "LooksLikeProductCode",
    "BuildProductDisplayName", "CreatePropertyArgument",
)


def extract(source, pattern, description):
    matches = list(re.finditer(pattern, source, re.M | re.S))
    if len(matches) != 1:
        raise RuntimeError("Cannot locate single production " + description)
    return matches[0].group()


def run(source_path=None):
    source = (source_path or HERE.parent / "VibeshineInstaller.cs").read_text(encoding="utf-8")
    namespace = "VibepolloInstaller" if "namespace VibepolloInstaller" in source else "VibeshineInstaller"
    methods = [extract(source, r"^    (?:private|public|internal) static [^\r\n]+\b" + name + r"\(.*?^    \}", name)
               for name in PRODUCTION_METHODS]
    types = [extract(source, r"^    internal (?:sealed class|enum) " + name + r"[^\r\n]*\{.*?^    \}", name)
             for name in ("InstalledProductInfo", "PayloadMsiInfo", "StashedVibeshinePayload", "InstalledProductKind")]
    result = extract(source, r"^  internal sealed class InstallerResult \{.*?^  \}", "InstallerResult")
    operation = extract(source, r"^  internal enum InstallerOperation \{.*?^  \}", "InstallerOperation")
    boundaries = (HERE / "rollback_boundary_methods.cs.in").read_text(encoding="utf-8")
    names = re.findall(r"^    (?:private|public) static [^\r\n]+\b(\w+)\(", boundaries, re.M)
    if len(names) != 11 or set(names) & set(PRODUCTION_METHODS):
        raise RuntimeError("External boundary list changed or includes production methods")
    for name in names:
        parameters = r"\(string msiPath\)" if name == "TryGetPayloadMsiInfo" else r"\("
        if name == "GetInstalledVibepolloProduct" and not re.search(r"\b" + name + r"\(", source):
            boundaries = re.sub(r"^    public static InstalledProductInfo " + name + r"\(\) \{.*?^    \}", "", boundaries, flags=re.M | re.S)
            continue
        extract(source, r"^    (?:private|public) static [^\r\n]+\b" + name + parameters + r".*?^    \}", name)
    portable = "using System;\nusing System.IO;\nusing System.Linq;\nusing System.Collections.Generic;\nnamespace " + namespace + " {\n"
    portable += result + "\n" + operation + "\ninternal static class InstallerRunner {\n"
    portable += "\n".join(types + methods) + "\n" + boundaries + "\n}\n}\n"
    harness = (HERE / "rollback_regression.cs").read_text(encoding="utf-8").replace("PRODUCT_NAMESPACE", namespace)
    with tempfile.TemporaryDirectory(prefix="vibeshine-rollback-test-") as directory:
        path = pathlib.Path(directory)
        (path / "Test.csproj").write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework></PropertyGroup></Project>')
        (path / "NuGet.Config").write_text('<configuration><packageSources><clear /></packageSources></configuration>')
        (path / "Production.cs").write_text(portable, encoding="utf-8")
        (path / "Regression.cs").write_text(harness, encoding="utf-8")
        env = dict(os.environ, DOTNET_CLI_HOME=str(path / "dotnet-home"),
                   DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1", DOTNET_CLI_TELEMETRY_OPTOUT="1",
                   DOTNET_GENERATE_ASPNET_CERTIFICATE="false", DOTNET_NOLOGO="1")
        subprocess.run([sys.argv[1] if len(sys.argv) > 1 else "dotnet", "run", "--project", str(path / "Test.csproj")], env=env, check=True)


if __name__ == "__main__":
    run()
