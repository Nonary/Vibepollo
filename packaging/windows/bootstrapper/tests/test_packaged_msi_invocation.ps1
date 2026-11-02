# Validate the production test's reflection call without running Windows MSI.
$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'test_packaged_msi_preflight.ps1')
$pattern = '(?m)^\s*\[object\[\]\]\$invokeArguments = [^\r\n]+\r?\n\s*\$result = \$method\.Invoke[^\r\n]+'
$call = [regex]::Matches($source, $pattern)
if ($call.Count -ne 1) { throw 'Expected one packaged-MSI reflection call' }
Add-Type -TypeDefinition @'
using System;
using System.Management.Automation;
public static class PreflightInvocationFixture {
    public static object[] Wrapped() {
        return new object[] { new PSObject("fixture.msi"), new PSObject("fixture-root") };
    }
    public static string Run(string msi, string root, string phase, string shortFileNames) {
        if (msi != "fixture.msi" || root != "fixture-root" || phase != "packaged_msi_preflight" || shortFileNames != null)
            throw new Exception("Incorrect reflection arguments");
        return "ok";
    }
}
'@
$method = [PreflightInvocationFixture].GetMethod('Run')
$wrapped = [PreflightInvocationFixture]::Wrapped()
$MsiPath = $wrapped[0]
$case = @{ Root=$wrapped[1] }
. ([scriptblock]::Create($call[0].Value))
if ($result -ne 'ok') { throw 'Packaged-MSI reflection call did not preserve strings' }
Write-Host 'Packaged-MSI reflection arguments passed.'
