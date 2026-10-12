param([Parameter(Mandatory=$true)][string]$OutputDirectory, [string]$Published200MsiPath = '')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$source = Join-Path $repo 'packaging/windows/bootstrapper/VibeshineInstaller.cs'
$namespace = if ((Get-Content -LiteralPath $source -Raw).Contains('namespace VibepolloInstaller')) { 'VibepolloInstaller' } else { 'VibeshineInstaller' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$harness = Join-Path $OutputDirectory 'migration-regression.cs'
[IO.File]::WriteAllText($harness, (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'migration_regression.cs') -Raw -Encoding UTF8).Replace('PRODUCT_NAMESPACE', $namespace), [Text.UTF8Encoding]::new($false))
$framework = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319'
$output = Join-Path $OutputDirectory 'migration-regression.exe'
$compiler = @('/nologo','/target:exe','/main:MigrationRegression',"/out:$output")
foreach ($reference in @('System.dll','System.Core.dll','System.Data.dll','System.Xml.dll','System.Xaml.dll','System.Windows.Forms.dll','WPF/WindowsBase.dll','WPF/PresentationCore.dll','WPF/PresentationFramework.dll')) { $compiler += '/reference:' + (Join-Path $framework $reference) }
$compiler += @($source, $harness)
& (Join-Path $framework 'csc.exe') @compiler
if ($LASTEXITCODE -ne 0) { throw 'Production bootstrap migration compilation failed' }
$wix = Join-Path ${env:ProgramFiles(x86)} 'WiX Toolset v3.14/bin'
$fixtureMsi = $null
if (Test-Path -LiteralPath (Join-Path $wix 'candle.exe')) {
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'payload.txt'), 'Metadata fixture only; never install.')
    $fixtureSource = @'
<?xml version="1.0" encoding="utf-8"?>
<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi">
  <Product Id="{11111111-1111-1111-1111-111111111111}" Name="Bootstrap metadata fixture - never install" Language="1033" Version="2.0.90" Manufacturer="Test" UpgradeCode="{33333333-3333-3333-3333-333333333333}">
    <Package InstallerVersion="500" Compressed="yes" InstallScope="perMachine"/>
    <MediaTemplate EmbedCab="yes"/>
    <Directory Id="TARGETDIR" Name="SourceDir"><Directory Id="ProgramFilesFolder"><Directory Id="INSTALL_ROOT" Name="FixtureNeverInstall">
      <Component Id="HostExecutable" Guid="{22222222-2222-2222-2222-222222222222}"><File Id="Host" Name="sunshine.exe" Source="$(var.FixtureRoot)/payload.txt" KeyPath="yes"/></Component>
    </Directory></Directory></Directory>
    <Feature Id="Application" Level="1"><ComponentRef Id="HostExecutable"/></Feature>
    <CustomAction Id="RemoveVirtualGamepadRoot" Property="FIXTURE_MARKER" Value="Metadata only"/>
    <InstallExecuteSequence><Custom Action="RemoveVirtualGamepadRoot" After="InstallInitialize">REMOVE AND NOT UPGRADINGPRODUCTCODE</Custom></InstallExecuteSequence>
  </Product>
</Wix>
'@
    $fixtureWxs = Join-Path $OutputDirectory 'metadata-fixture.wxs'
    [IO.File]::WriteAllText($fixtureWxs, $fixtureSource)
    & (Join-Path $wix 'candle.exe') -nologo "-dFixtureRoot=$OutputDirectory" -out (Join-Path $OutputDirectory 'metadata-fixture.wixobj') $fixtureWxs
    if ($LASTEXITCODE -ne 0) { throw 'Metadata fixture WiX compile failed' }
    $fixtureMsi = Join-Path $OutputDirectory 'metadata-fixture.msi'
    & (Join-Path $wix 'light.exe') -nologo -sval -out $fixtureMsi (Join-Path $OutputDirectory 'metadata-fixture.wixobj')
    if ($LASTEXITCODE -ne 0) { throw 'Metadata fixture WiX link failed' }
    $unsafeMsi = Join-Path $OutputDirectory 'metadata-unsafe-fixture.msi'
    Copy-Item -LiteralPath $fixtureMsi -Destination $unsafeMsi -Force
    # Change a copy's table only; never install/register the metadata fixture.
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.OpenDatabase([IO.Path]::GetFullPath($unsafeMsi), 1)
    $view = $database.OpenView("UPDATE ``InstallExecuteSequence`` SET ``Condition``='REMOVE' WHERE ``Action``='RemoveVirtualGamepadRoot'")
    $null = $view.Execute(); $null = $view.Close(); $null = $database.Commit()
    $null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
    $null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($database)
    $null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
}
if ($Published200MsiPath) { $Published200MsiPath = [IO.Path]::GetFullPath($Published200MsiPath) }
if ($fixtureMsi) { & $output ([IO.Path]::GetFullPath($fixtureMsi)) ([IO.Path]::GetFullPath($unsafeMsi)) $Published200MsiPath } else { & $output }
if ($LASTEXITCODE -ne 0) { throw 'Bootstrap migration regression failed' }
[xml]$patch = Get-Content -LiteralPath (Join-Path $repo 'packaging/windows/wix/patch_custom_actions.wxs') -Raw
[xml]$custom = Get-Content -LiteralPath (Join-Path $repo 'packaging/windows/wix/custom_actions.wxs') -Raw
if ($patch.SelectNodes('//Custom[@Action="AskUninstallLegacySunshine" or @Action="WaitForLegacyUninstall" or @Action="BlockUserCancelledLegacy"]').Count) { throw 'MSI must not launch or poll legacy uninstallers' }
$ns = New-Object Xml.XmlNamespaceManager($custom.NameTable)
$ns.AddNamespace('w', 'http://schemas.microsoft.com/wix/2006/wi')
if ($custom.SelectNodes('//w:CustomAction[@Id="AskUninstallLegacySunshine" or @Id="WaitForLegacyUninstall"]', $ns).Count) { throw 'Unsafe legacy MSI actions remain authored' }
foreach ($action in @('MigrateConfig','SetMigrateConfig','RunInstallerMigrations','SetRunInstallerMigrations','RemoveConflictingProducts','BlockLegacySunshineStillPresent')) {
    if ($patch.SelectNodes("//Custom[@Action='$action']").Count -or $custom.SelectNodes("//w:CustomAction[@Id='$action']", $ns).Count) { throw "MSI must not own migration: $action" }
}
if ($custom.SelectNodes('//w:RemoveRegistryKey[contains(@Id,"RemoveLegacyApolloUninstallKey")]', $ns).Count -or $patch.SelectNodes('//ComponentRef[@Id="RemoveLegacyApolloUninstallKey"]').Count) { throw 'MSI must not blindly retire legacy ARP keys' }
Write-Output 'Bootstrap-only migration ownership and removal of MSI workaround/shims checks passed.'
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('migration-junction-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $fixture 'target') -Force | Out-Null
$junction = Join-Path $fixture 'link'
try {
    New-Item -ItemType Junction -Path $junction -Target (Join-Path $fixture 'target') | Out-Null
    $assembly = [Reflection.Assembly]::Load([IO.File]::ReadAllBytes($output))
    $type = $assembly.GetType($namespace + '.InstallerRunner+LegacyDataCheckpoint')
    $method = $type.GetMethod('AssertNoReparsePoints', [Reflection.BindingFlags]'Static,NonPublic')
    $rejected = $false
    $guardArguments = New-Object 'System.Object[]' 1
    $guardArguments[0] = [string](Join-Path $junction 'nested/path')
    try { $null = $method.Invoke($null, $guardArguments) }
    catch { if ($_.Exception.InnerException -is [InvalidOperationException]) { $rejected = $true } else { throw } }
    if (!$rejected) { throw 'Migration must reject reparse point ancestors' }
    Write-Output 'Production path guard rejects junction traversal (temporary fixture only).'
} finally {
    if (Test-Path -LiteralPath $junction) { [IO.Directory]::Delete($junction, $false) }
    $resolved = [IO.Path]::GetFullPath($fixture)
    if (!$resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
