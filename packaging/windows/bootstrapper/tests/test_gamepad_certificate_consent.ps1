$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$sourcePath = Join-Path $repo 'packaging/windows/bootstrapper/VibeshineInstaller.cs'
$source = Get-Content -LiteralPath $sourcePath -Raw
$framework = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319'
$assemblyPath = Join-Path ([IO.Path]::GetTempPath()) ('gamepad-consent-contract-' + [Guid]::NewGuid().ToString('N') + '.exe')
$compilerArgs = @('/nologo', '/target:winexe', "/out:$assemblyPath")
foreach ($reference in @('System.dll', 'System.Core.dll', 'System.Data.dll', 'System.Xml.dll', 'System.Xaml.dll', 'System.Windows.Forms.dll', 'WPF/WindowsBase.dll', 'WPF/PresentationCore.dll', 'WPF/PresentationFramework.dll')) {
    $compilerArgs += '/reference:' + (Join-Path $framework $reference)
}
$compilerArgs += $sourcePath
& (Join-Path $framework csc.exe) @compilerArgs
if ($LASTEXITCODE -ne 0) { throw 'Consent bootstrapper compilation failed.' }
try {
    # Load metadata and invoke only argument construction; never run the installer/UI.
    $assembly = [Reflection.Assembly]::Load([IO.File]::ReadAllBytes($assemblyPath))
    $namespaceName = [regex]::Match($source, 'namespace (\w+)').Groups[1].Value
    $argumentType = $assembly.GetType("$namespaceName.InstallerArguments", $true)
    $runnerType = $assembly.GetType("$namespaceName.InstallerRunner", $true)
    $parse = $argumentType.GetMethod('Parse', [Reflection.BindingFlags]'Public,Static')
    $construct = $runnerType.GetMethod('BuildLocalGamepadTrustArguments', [Reflection.BindingFlags]'NonPublic,Static')
    function Parse-TestArguments([string[]]$Values) {
        $parameters = New-Object object[] 1
        $parameters[0] = $Values
        return $parse.Invoke($null, $parameters)
    }
    function Assert-ConsentArguments($Arguments, [bool]$InstallGamepad, [bool]$Expected) {
        $parameters = New-Object object[] 2
        $parameters[0] = $Arguments
        $parameters[1] = $InstallGamepad
        $actual = @($construct.Invoke($null, $parameters))
        $flag = if ($Expected) { '1' } else { '0' }
        if ($actual[0] -cne "ALLOW_LOCAL_GAMEPAD_CERTIFICATE_TRUST=$flag") { throw 'Consent defaults or install gating changed.' }
        $identity = if ($Expected) { 'A' * 40 } else { 'none' }
        if ($actual[1] -cne "LOCAL_GAMEPAD_CERTIFICATE_THUMBPRINT=$identity") { throw 'Consent is not bound to the exact certificate identity.' }
    }
    $arguments = Parse-TestArguments @('/qn')
    Assert-ConsentArguments $arguments $true $false
    $arguments = Parse-TestArguments @('--ui', 'ALLOW_LOCAL_GAMEPAD_CERTIFICATE_TRUST=1')
    Assert-ConsentArguments $arguments $true $false
    $arguments = Parse-TestArguments @('--internal-elevated-install', '--internal-gamepad-certificate-trust', '1', '--internal-gamepad-consent-thumbprint', ('A' * 40))
    Assert-ConsentArguments $arguments $true $true
    Assert-ConsentArguments $arguments $false $false
    $arguments.ConsentCertificateThumbprint = 'different-or-malformed'
    Assert-ConsentArguments $arguments $true $false
    $arguments = Parse-TestArguments @('--internal-elevated-install')
    Assert-ConsentArguments $arguments $true $false

    if ($source.Contains('_localGamepadCertificateTrustCheckBox')) { throw 'Separate beta certificate checkbox must be removed.' }
    foreach ($disclosure in @('verified bundled signing certificate', 'Equal or newer installed drivers remain unchanged', 'Windows security enforcement stays enabled')) {
        if (-not $source.Contains($disclosure)) { throw "Missing concise beta disclosure: $disclosure" }
    }
    [xml]$wix = Get-Content (Join-Path $repo 'packaging/windows/wix/custom_actions.wxs') -Raw
    $namespace = New-Object Xml.XmlNamespaceManager($wix.NameTable)
    $namespace.AddNamespace('w', 'http://schemas.microsoft.com/wix/2006/wi')
    $property = $wix.SelectSingleNode('//w:Property[@Id="ALLOW_LOCAL_GAMEPAD_CERTIFICATE_TRUST"]', $namespace)
    if ($property.Value -cne '$(var.VhfGamepadAllowLocalTest)' -or $property.Secure -cne 'yes') { throw 'MSI beta trust must follow verified local package presence, remain secure and preserve identity binding.' }
    $action = $wix.SelectSingleNode('//w:CustomAction[@Id="SetInstallVirtualGamepadDriver"]', $namespace)
    if (-not $action.Value.Contains('-AllowLocalGamepadCertificateTrust [ALLOW_LOCAL_GAMEPAD_CERTIFICATE_TRUST]') -or -not $action.Value.Contains('-ConsentCertificateThumbprint "[LOCAL_GAMEPAD_CERTIFICATE_THUMBPRINT]"')) { throw 'MSI gamepad action lost explicit consent/identity binding.' }
    $reader = Get-Content (Join-Path $repo 'scripts/read_gamepad_certificate_identity.ps1') -Raw
    $contentCheck = $reader.IndexOf('GetCertContentType($certificateBytes)')
    $certificateLoad = $reader.IndexOf('X509Certificate2]::new($certificateBytes)')
    if ($contentCheck -lt 0 -or $certificateLoad -lt $contentCheck -or $reader -notmatch 'X509ContentType\]::Cert') { throw 'Public certificate content classification must precede certificate construction; reject PFX/PKCS12 without importing keys.' }
    foreach ($policy in @('CertificateAuthority', '1.3.6.1.5.5.7.3.3', 'NotBefore.ToUniversalTime()', 'NotAfter.ToUniversalTime()', 'signer_thumbprint', 'device_setup_signer_thumbprint')) {
        if (-not $reader.Contains($policy)) { throw "Missing read-only certificate policy: $policy" }
    }
    Write-Host 'Bootstrapper consent defaults, identity binding, elevated argument roundtrip and MSI contract passed. No UI, installers or trust changes executed.'
} finally {
    Remove-Item -LiteralPath $assemblyPath -Force -ErrorAction SilentlyContinue
}
