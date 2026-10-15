# Runs on PowerShell 5.1/7, including Linux: exercises package validation with
# temporary fixtures and compiles the Windows SetupAPI wrapper without calling
# any native Windows API or installing a driver.
$ErrorActionPreference = 'Stop'
$packageRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $packageRoot '..\..\..')).Path
. (Join-Path $packageRoot 'package.ps1')
$signatureStatus = 'Valid'
$signatureSubject = 'CN=Microsoft Windows Hardware Compatibility Publisher, O=Microsoft Corporation, C=US'
function Get-AuthenticodeSignature {
    param([string]$LiteralPath)
    [pscustomobject]@{ Status=$signatureStatus; SignerCertificate=[pscustomobject]@{ Subject=$signatureSubject } }
}
function Expect-Failure([scriptblock]$Action, [string]$Message) {
    $failed = $false
    try { & $Action } catch {
        if ($_.Exception.Message -notlike "*$Message*") { throw "Unexpected failure: $($_.Exception.Message)" }
        $failed = $true
    }
    if (!$failed) { throw "Expected rejection: $Message" }
}
function Write-Lock {
    $lock | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $fixture 'release-lock.json') -Encoding UTF8
}

$fixture = Join-Path ([IO.Path]::GetTempPath()) ('vibepollo-usbip-test-' + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $fixture | Out-Null
    $hashes = [ordered]@{}
    foreach ($name in $UsbIpFiles) {
        Set-Content -LiteralPath (Join-Path $fixture $name) -Value "fixture $name" -Encoding UTF8
        $hashes[$name] = (Get-FileHash -LiteralPath (Join-Path $fixture $name) -Algorithm SHA256).Hash
    }
    # Substitute fixture pins only for executing the real validation functions.
    $UsbIpUdeHashes = @{}
    foreach ($name in @('usbip2_ude.inf', 'usbip2_ude.sys', 'usbip2_ude.cat')) { $UsbIpUdeHashes[$name] = $hashes[$name] }
    $lock = [ordered]@{ version=$UsbIpVersion; tag=$UsbIpReleaseTag; architecture='x64'; installer_sha256=$UsbIpInstallerHash; files=$hashes }
    Write-Lock
    Assert-UsbIpPackage $fixture

    $lock.architecture = 'arm64'; Write-Lock
    Expect-Failure { Assert-UsbIpPackage $fixture } 'pinned Microsoft-signed release'
    $lock.architecture = 'x64'
    $lock.installer_sha256 = 'bad'; Write-Lock
    Expect-Failure { Assert-UsbIpPackage $fixture } 'pinned Microsoft-signed release'
    $lock.installer_sha256 = $UsbIpInstallerHash; Write-Lock
    $signatureStatus = 'NotSigned'
    Expect-Failure { Assert-UsbIpPackage $fixture } 'valid Microsoft hardware publisher signature'
    $signatureStatus = 'Valid'; $signatureSubject = 'CN=USBip Test Certificate'
    Expect-Failure { Assert-UsbIpPackage $fixture } 'valid Microsoft hardware publisher signature'
    $signatureSubject = 'CN=Microsoft Windows Hardware Compatibility Publisher, O=Microsoft Corporation, C=US'

    $sys = Join-Path $fixture 'usbip2_ude.sys'
    Add-Content -LiteralPath $sys -Value 'tampered'
    Expect-Failure { Assert-UsbIpPackage $fixture } 'hash mismatch'
    $lock.files['usbip2_ude.sys'] = (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash; Write-Lock
    # Changing both the payload and manifest still fails the independent pin.
    Expect-Failure { Assert-UsbIpPackage $fixture } 'hash mismatch'

    foreach ($file in @((Join-Path $packageRoot 'install.ps1'),
                         (Join-Path $packageRoot 'package.ps1'),
                         (Join-Path $repoRoot 'scripts\download_usbip_transport.ps1'))) {
        $tokens = $null; $errors = $null
        $ast = [System.Management.Automation.Language.Parser]::ParseFile($file, [ref]$tokens, [ref]$errors)
        if ($errors.Count -gt 0) { throw "$file has PowerShell syntax errors: $($errors.Message -join '; ')" }
        if ($file.EndsWith('install.ps1')) {
            $command = $ast.Find({param($node) $node -is [System.Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Add-DriverInteropType'}, $true)
            Add-Type -TypeDefinition $command.CommandElements[2].Value
            foreach ($name in @('DeviceInfo', 'InterfaceInfo', 'InstallParams')) {
                $type = [VibepolloUsbIpSetup].GetNestedType($name, [Reflection.BindingFlags]::NonPublic)
                $expected = if ($name -eq 'InstallParams') {
                    if ([IntPtr]::Size -eq 8) { 584 } else { 556 }
                } else {
                    if ([IntPtr]::Size -eq 8) { 32 } else { 28 }
                }
                $sizeMethod = [Runtime.InteropServices.Marshal].GetMethod('SizeOf', [Type[]]@([Type]))
                if ($sizeMethod.Invoke($null, [object[]]@($type)) -ne $expected) { throw "Wrong SetupAPI ABI size: $name" }
            }
        }
    }
    Write-Output 'USB/IP package tamper, release pin, test signer rejection, PowerShell parsing, and SetupAPI wrapper compilation checks passed.'
} finally {
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}
