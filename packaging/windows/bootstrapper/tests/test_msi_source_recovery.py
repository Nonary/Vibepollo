#!/usr/bin/env python3
"""Execute production exact-MSI-source recovery with native/transport boundaries.

Requires .NET 8. This is a portable behavioral harness, not a Windows install,
Authenticode chain, Windows ACL, source-list, or rollback integration test.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
METHODS = (
    'GetInstalledProductRegistrations', 'BuildProductRegistrationIdentity', 'MergeInstalledProductGroup', 'NormalizeCommandIdentity', 'IsInstalledProductCode',
    'QueryMsiProductStateForRollback', 'ResolveInstalledProductFacades', 'TryGetFullPathOrNull', 'IsWindowsInstallerProduct', 'FindKnownMsiSourceRelease', 'FindKnownMsiSourceInCommand', 'CreateKnownMsiProductView', 'GetInstalledProductKind',
    'TryPreserveKnownMsiSourceBeforeReplacement', 'UninstallInstalledProducts', 'UninstallCompetingProducts',
    'MsiSourceIdentityMatches', 'ReadMachineMsiProperty', 'ReadMachineMsiSourceIdentity',
    'IsVerifiedRecoveryMsi', 'ExtractVerifiedRecoveryBundle', 'RecoverExactMsiSource',
    'RunMsiexecWithSourceRecovery', 'NormalizeProductCode', 'LooksLikeProductCode',
    'IsRecoveryKindAllowed', 'IsRecoverableMsiCacheFailure', 'LogShowsMsiCacheOrSourceFailure',
    'ReplaceArgumentValue', 'FileHashMatches', 'ComputeFileSha256Hex', 'ComputeStreamSha256Hex',
    'TryRepairBustedMsiRegistration', 'ShouldRepairBustedMsiRegistration',
)


def extract(source, pattern, name):
    found = list(re.finditer(pattern, source, re.M | re.S))
    if len(found) != 1:
        raise RuntimeError('Expected one production ' + name)
    return found[0].group()


def run():
    source = (HERE.parent / 'VibeshineInstaller.cs').read_text()
    methods = [extract(source, r'^    (?:private|internal) static [^\r\n]+\b' + name + r'\(.*?^    \}', name) for name in METHODS]
    types = [extract(source, r'^    internal (?:sealed class|enum) ' + name + r'[^\r\n]*\{.*?^    \}', name)
             for name in ('MsiSourceIdentity', 'MsiSourceRelease', 'InstalledProductInfo', 'InstalledProductKind', 'StashedVibeshinePayload')]
    result = extract(source, r'^  internal sealed class InstallerResult \{.*?^  \}', 'InstallerResult')
    operation = extract(source, r'^  internal enum InstallerOperation \{.*?^  \}', 'InstallerOperation')
    fields = [extract(source, r'^    private static readonly (?:MsiSourceRelease\[\]|InstalledProductKind\[\]|string\[\]) ' + name + r' = \{.*?^    \};', name)
              for name in ('RecoverableMsiSources', 'MsiRegistrationRecoveryKinds', 'MsiCacheFailureLogMarkers')]
    for name in ('MsiSourceRecoveryAttempts', 'MsiSourceRecoveryRetries', 'RecoveredMsiSources'):
        fields.append(extract(source, r'^    private static readonly [^\r\n]+\b' + name + r' = [^\r\n]+;', name))
    namespace = 'VibepolloInstaller' if 'namespace VibepolloInstaller' in source else 'VibeshineInstaller'
    code = '\n'.join(('using System;', 'using System.IO;', 'using System.Linq;', 'using System.Text;',
                      'using System.Collections.Generic;', 'using System.Security.Cryptography;',
                      'namespace ' + namespace + ' {', result, operation, 'internal static partial class InstallerRunner {',
                      *types, *fields, *methods, '}', '}'))
    harness = (HERE / 'msi_source_recovery_regression.cs').read_text().replace('PRODUCT_NAMESPACE', namespace)
    with tempfile.TemporaryDirectory(prefix='exact-msi-source-test-') as directory:
        path = pathlib.Path(directory)
        (path / 'Test.csproj').write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework></PropertyGroup></Project>')
        (path / 'NuGet.Config').write_text('<configuration><packageSources><clear /></packageSources></configuration>')
        (path / 'Production.cs').write_text(code)
        (path / 'Regression.cs').write_text(harness)
        env = dict(os.environ, DOTNET_CLI_HOME=str(path / 'home'), DOTNET_CLI_TELEMETRY_OPTOUT='1',
                   DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1', DOTNET_GENERATE_ASPNET_CERTIFICATE='false', DOTNET_NOLOGO='1')
        subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'dotnet', 'run', '--project', str(path / 'Test.csproj')], env=env, check=True)


if __name__ == '__main__':
    run()
