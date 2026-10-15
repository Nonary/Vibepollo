# Compile only the extracted interop wrappers. Never execute an installer,
# touch driver/certificate stores, or call any of the native wrapper methods.
param([string]$RepositoryRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop') {
    throw 'Run this regression with Windows PowerShell 5.1 to exercise the .NET Framework CodeDOM limit.'
}
$paths = @(
    'src_assets/windows/drivers/vhf-gamepad/install.ps1',
    'packaging/windows/usbip_transport/install.ps1'
)
$definitions = @()
$commands = @()
foreach ($path in $paths) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $RepositoryRoot $path), [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors.Message -join '; ') }
    $function = $ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Add-DriverInteropType'}, $true)
    if (!$function) { throw "Missing interop compiler in $path" }
    $definitions += $function.Extent.Text
    $command = $ast.Find({param($n) $n -is [Management.Automation.Language.CommandAst] -and $n.GetCommandName() -eq 'Add-DriverInteropType'}, $true)
    if (!$command) { throw "Missing production interop compilation in $path" }
    $commands += $command
}
if ($definitions[0] -cne $definitions[1]) { throw 'Standalone installer compiler helpers have diverged.' }

function Assert-EnvironmentUnchanged($Before) {
    $after = [Environment]::GetEnvironmentVariables('Process')
    if ($Before.Count -ne $after.Count) { throw 'Interop compilation changed the process environment size.' }
    foreach ($key in $Before.Keys) {
        if ($Before[$key] -cne $after[$key]) { throw 'Interop compilation changed a process environment variable.' }
    }
}
$original = [Environment]::GetEnvironmentVariables('Process')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('driver env ' + [char]0x00e9 + [char]0x4e2d + '-' + [Guid]::NewGuid().ToString('N'))
$prefix = 'DRIVER_ENV_' + [Guid]::NewGuid().ToString('N')
$added = @()
try {
    $null = New-Item -ItemType Directory -Path $fixture
    # Establish that CodeDOM works before changing the environment. Keep its
    # original scratch path for the size-only negative control: combining the
    # Unicode scratch fixture with that control can fail earlier with Win32 123.
    Add-Type -TypeDefinition ('public class CodeDomControl' + [Guid]::NewGuid().ToString('N') + ' {}')
    Assert-EnvironmentUnchanged $original
    # Each value remains below Windows' per-variable limit, while the total
    # exceeds both 65535 and the 170393-byte environment reported in #610.
    foreach ($i in 0..11) {
        $key = $prefix + '_' + $i; $added += $key
        [Environment]::SetEnvironmentVariable($key, ('x' * 16000), 'Process')
    }
    $large = [Environment]::GetEnvironmentVariables('Process')
    $characters = 1
    foreach ($name in $large.Keys) { $characters += $name.Length + $large[$name].Length + 2 }
    if ($characters -le 170393) { throw 'Fixture environment is too small to reproduce #610.' }

    $baselineFailed = $false
    try {
        Add-Type -TypeDefinition ('public class CodeDomBaseline' + [Guid]::NewGuid().ToString('N') + ' {}')
    } catch {
        # The fixed numeric limit is present even when Framework localizes the message.
        if ($_.Exception.ToString() -notmatch '65535') { throw }
        $baselineFailed = $true
    }
    if (!$baselineFailed) { throw 'The original Add-Type path did not reproduce the Framework environment limit.' }
    Assert-EnvironmentUnchanged $large

    # The replacement must also preserve Unicode values and compile beneath a
    # scratch root containing spaces and Unicode. Exercise those independently
    # of the original compiler's size-only negative control.
    [Environment]::SetEnvironmentVariable('TEMP', $fixture, 'Process')
    [Environment]::SetEnvironmentVariable('TMP', $fixture, 'Process')
    $key = $prefix + '_UNICODE'; $added += $key
    [Environment]::SetEnvironmentVariable($key, "C:\Users\$([char]0x00e9)$([char]0x4e2d)\Driver Test", 'Process')
    $large = [Environment]::GetEnvironmentVariables('Process')

    foreach ($i in 0..1) {
        . ([scriptblock]::Create($definitions[$i]))
        # Invoke just the actual compile expression, outside the installer body.
        & ([scriptblock]::Create($commands[$i].Extent.Text))
        Assert-EnvironmentUnchanged $large
        $failed = $false
        try { Add-DriverInteropType -TypeDefinition 'this is deliberately invalid C#' } catch {
            if ($_.Exception.Message -notlike 'Driver interop compilation failed (exit *') { throw }
            $failed = $true
        }
        if (!$failed) { throw 'Compiler failure was hidden.' }
        Assert-EnvironmentUnchanged $large
    }
    if (!('VhfLocalTrustValidation' -as [type]) -or !('VibepolloUsbIpSetup' -as [type])) {
        throw 'The production interop types were not loaded.'
    }
    if (@(Get-ChildItem -LiteralPath $fixture -Force).Count) { throw 'Compiler scratch was not removed.' }
    Write-Output 'Reproduced the Add-Type 65535-byte failure; both production wrappers compiled with the full oversized Unicode environment. Compiler errors remained visible and scratch was removed.'
} finally {
    foreach ($name in $added) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    foreach ($name in @('TEMP', 'TMP')) { [Environment]::SetEnvironmentVariable($name, $original[$name], 'Process') }
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
    Assert-EnvironmentUnchanged $original
}
