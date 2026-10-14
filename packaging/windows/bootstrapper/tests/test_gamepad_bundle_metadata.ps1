param([Parameter(Mandatory=$true)][string]$RegressionExe, [Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
[xml]$patch = Get-Content (Join-Path $repo 'packaging/windows/wix/patch_custom_actions.wxs') -Raw
[xml]$custom = Get-Content (Join-Path $repo 'packaging/windows/wix/custom_actions.wxs') -Raw
$ns = New-Object Xml.XmlNamespaceManager($custom.NameTable)
$ns.AddNamespace('w', 'http://schemas.microsoft.com/wix/2006/wi')
$capability = $patch.SelectSingleNode('//Property[@Id="VhfGamepadDriverBundled"]')
if ($null -eq $capability -or $capability.Value -cne '$(var.VhfGamepadDriverBundled)') { throw 'Missing build-time bundle capability.' }
$selection = $patch.SelectSingleNode('//Property[@Id="INSTALL_VIRTUAL_GAMEPAD_DRIVER"]')
if ($null -eq $selection -or $selection.HasAttribute('Value')) { throw 'Persisted selection must have no static default.' }
$local = $patch.SelectSingleNode('//Property[@Id="ALLOW_LOCAL_VIRTUAL_GAMEPAD_TEST_PACKAGE"]')
$certificate = $custom.SelectSingleNode('//w:Property[@Id="LOCAL_GAMEPAD_CERTIFICATE_THUMBPRINT"]', $ns)
$candle = Get-Command candle.exe -ErrorAction SilentlyContinue
$wix = if ($null -ne $candle) { Split-Path -Parent $candle.Source } else { Join-Path ${env:ProgramFiles(x86)} 'WiX Toolset v3.14/bin' }
if (!(Test-Path -LiteralPath (Join-Path $wix 'candle.exe'))) { throw 'WiX required for actual gamepad MSI metadata fixtures.' }
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'gamepad-metadata-payload.txt'), 'Metadata fixture only; never install.')
$template = @'
<?xml version="1.0" encoding="utf-8"?>
<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi">
  <Product Id="*" Name="Gamepad metadata fixture - never install" Language="1033" Version="2.0.100" Manufacturer="Test" UpgradeCode="{33333333-3333-3333-3333-333333333333}">
    <Package InstallerVersion="500" Compressed="yes" InstallScope="perMachine"/>
    <MediaTemplate EmbedCab="yes"/>
    FIXTURE_PROPERTIES
    <Directory Id="TARGETDIR" Name="SourceDir"><Directory Id="ProgramFilesFolder"><Directory Id="INSTALL_ROOT" Name="FixtureNeverInstall">
      <Component Id="Payload" Guid="*"><File Id="PayloadFile" Source="$(var.FixtureRoot)/gamepad-metadata-payload.txt" KeyPath="yes"/></Component>
    </Directory></Directory></Directory>
    <Feature Id="Application" Level="1"><ComponentRef Id="Payload"/></Feature>
  </Product>
</Wix>
'@
$fixtures = @()
foreach ($kind in @('bundled', 'absent', 'missing', 'malformed')) {
    # Import actual authoring, including the deliberately valueless selection.
    # Negative fixtures simulate an absent/old/invalid capability while a mutable
    # selection of 1 proves it cannot stand in for package contents.
    $properties = $selection.OuterXml + $local.OuterXml + $certificate.OuterXml
    $bundled = '1'
    if ($kind -ne 'missing') {
        $node = $capability.CloneNode($true)
        if ($kind -eq 'absent') { $bundled = '0' }
        if ($kind -eq 'malformed') { $node.SetAttribute('Value', 'unknown') }
        $properties += $node.OuterXml
    }
    if ($kind -ne 'bundled') { $properties = $properties.Replace($selection.OuterXml, '<Property Id="INSTALL_VIRTUAL_GAMEPAD_DRIVER" Value="1" Secure="yes"/>') }
    $base = Join-Path $OutputDirectory ('gamepad-metadata-' + $kind)
    [IO.File]::WriteAllText(($base + '.wxs'), $template.Replace('FIXTURE_PROPERTIES', $properties))
    & (Join-Path $wix 'candle.exe') -nologo "-dFixtureRoot=$OutputDirectory" "-dVhfGamepadDriverBundled=$bundled" -dVhfGamepadAllowLocalTest=1 -dLocalGamepadCertificateThumbprint=7F1868BF86698A8674F40E686B02C8C702BD3B88 -out ($base + '.wixobj') ($base + '.wxs')
    if ($LASTEXITCODE -ne 0) { throw "Gamepad metadata fixture compile failed: $kind" }
    & (Join-Path $wix 'light.exe') -nologo -sval -out ($base + '.msi') ($base + '.wixobj')
    if ($LASTEXITCODE -ne 0) { throw "Gamepad metadata fixture link failed: $kind" }
    $fixtures += $base + '.msi'
}
# The production reader opens metadata with MsiOpenPackageIgnoreMachineState.
# Never install the fixtures, execute custom actions, or modify trust/registry.
& $RegressionExe @fixtures
if ($LASTEXITCODE -ne 0) { throw 'Actual gamepad bundle metadata regression failed.' }
