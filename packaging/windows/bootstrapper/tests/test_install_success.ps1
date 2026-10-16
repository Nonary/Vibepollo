# Exercise the actual USB/IP orchestration with mocked OS/driver boundaries,
# then compile the complete production bootstrapper and test its finish UI.
param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$scriptPath = Join-Path $repo 'packaging/windows/usbip_transport/install.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors.Message -join '; ') }
$outer = $ast.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.TryStatementAst] }
$inner = $outer.Body.Statements | Where-Object { $_ -is [Management.Automation.Language.TryStatementAst] }
if (@($outer).Count -ne 1 -or @($inner).Count -ne 1) { throw 'USB/IP orchestration structure changed; review fixture extraction.' }
$warning = $ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Write-UsbIpWarning'}, $true).Extent.Text
# Run production statements inside a child so its real exit statements are tested.
# Prerequisite validation/identity/SetupAPI are deliberately excluded. The existing
# package-contract suite separately executes signature/hash validation and compiles
# the real SetupAPI wrapper. Every side-effecting boundary below is a fail-closed mock.
$stub = @'
param([string]$Scenario)
$ErrorActionPreference = 'Stop'
$InstallerBestEffort = $Scenario -ne 'staging-failed-strict'
$UsbIpInstallerHash = 'fixture-pin'
Add-Type -TypeDefinition @"
public static class MockUsbIpSetup {
 public static bool Shared, Ready, Reboot;
 public static bool HasRootDevice() { return Shared; }
 public static bool Available() { return Ready; }
 public static bool CreateRoot() { System.Console.WriteLine("MOCK_CREATE_ROOT"); return Reboot; }
}
"@
[MockUsbIpSetup]::Shared = $Scenario.StartsWith('shared-')
[MockUsbIpSetup]::Ready = $Scenario -eq 'shared-ready' -or $Scenario -eq 'new-ready' -or $Scenario -eq 'new-reboot'
[MockUsbIpSetup]::Reboot = $Scenario -eq 'new-reboot'
$locked = $false
$mutex = New-Object PSObject
$mutex | Add-Member ScriptMethod WaitOne {param($timeout) return $true}
$mutex | Add-Member ScriptMethod ReleaseMutex {}
$mutex | Add-Member ScriptMethod Dispose {}
function Test-Path {param($Path) if ($Path -notlike 'HKLM:*usbip2_*') { throw "Unexpected read: $Path" }; return [MockUsbIpSetup]::Shared }
function Get-ItemProperty {param($LiteralPath,$ErrorAction) if ($LiteralPath -notlike 'HKLM:*UsbIpInstall') { throw 'Unexpected state read' }; return $null }
function Get-Content {param($LiteralPath,[switch]$Raw,$Encoding) if ($LiteralPath -notlike '*release-lock.json') { throw 'Unexpected file read' }; return '{"files":{}}' }
function Get-ChildItem {param($LiteralPath,$Filter) if ($Filter -ne 'oem*.inf') { throw 'Unexpected enumeration' }; return @() }
function New-Item {param($Path,[switch]$Force) if ($Path -notlike 'HKLM:*UsbIpInstall') { throw 'Unexpected state write' }; Write-Output 'MOCK_STATE_WRITE' }
function New-ItemProperty {param($LiteralPath,$Name,$Value,$PropertyType,[switch]$Force) if ($LiteralPath -notlike 'HKLM:*UsbIpInstall') { throw 'Unexpected state write' }; Write-Output 'MOCK_STATE_WRITE' }
function Remove-Item {param($LiteralPath,[switch]$Force) if ($LiteralPath -notlike 'HKLM:*UsbIpInstall') { throw 'Unexpected state removal' }; Write-Output 'MOCK_STATE_REMOVE' }
function Join-Path {param($Path,$ChildPath) if ($ChildPath -eq 'System32\pnputil.exe') { return 'Mock-PnpUtil' }; return "$Path/$ChildPath" }
function Mock-PnpUtil { Write-Output 'MOCK_STAGE'; $global:LASTEXITCODE = if ($Scenario.StartsWith('staging-failed')) { 5 } else { 0 } }
function Start-Sleep {param($Milliseconds)}
'@
$body = $inner.Extent.Text.Replace('[VibepolloUsbIpSetup]', '[MockUsbIpSetup]')
$fixture = Join-Path $output 'usbip-fixture.ps1'
[IO.File]::WriteAllText($fixture, $stub + "`n" + $warning + "`ntry {`n" + $body + "`n} " + $outer.CatchClauses[0].Extent.Text)
$shell = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
foreach ($scenario in @('shared-unavailable','shared-ready','new-ready','new-not-ready','new-reboot','staging-failed','staging-failed-strict')) {
    $lines = & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $fixture $scenario 2>&1
    $code = $LASTEXITCODE
    $log = $lines -join "`n"
    [IO.File]::WriteAllText((Join-Path $output "$scenario.log"), $log)
    $expected = if ($scenario -eq 'staging-failed-strict') { 1 } else { 0 }
    if ($code -ne $expected) { throw "$scenario exit $code expected $expected`: $log" }
    if ($scenario.StartsWith('shared-') -and $log -match 'MOCK_(STAGE|CREATE_ROOT|STATE_WRITE)') { throw 'Shared drivers must remain unchanged.' }
    if ($scenario -in @('shared-unavailable','new-not-ready') -and ($log -notmatch 'USBIP_TRANSPORT_INFO' -or $log -match 'USBIP_TRANSPORT_WARNING')) { throw "$scenario must retain informational readiness detail." }
    if ($scenario.StartsWith('staging-failed') -and $log -notmatch 'USBIP_TRANSPORT_WARNING.*staging failed') { throw 'Actual staging failure diagnostic lost.' }
}
$framework = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319'
$harness = Join-Path $output 'regression.cs'
(Get-Content (Join-Path $PSScriptRoot 'install_success_regression.cs') -Raw).Replace('PRODUCT_NAMESPACE', 'VibepolloInstaller') | Set-Content $harness
$exe = Join-Path $output 'install-success-regression.exe'
$compile = @('/nologo','/target:exe','/main:InstallSuccessRegression',"/out:$exe")
foreach ($reference in @('System.dll','System.Core.dll','System.Data.dll','System.Xml.dll','System.Xaml.dll','System.Windows.Forms.dll','WPF/WindowsBase.dll','WPF/PresentationCore.dll','WPF/PresentationFramework.dll')) { $compile += '/reference:' + (Join-Path $framework $reference) }
$compile += @((Join-Path $repo 'packaging/windows/bootstrapper/VibeshineInstaller.cs'), $harness)
& (Join-Path $framework 'csc.exe') @compile
if ($LASTEXITCODE -ne 0) { throw 'Production bootstrapper compilation failed.' }
& $exe $output
if ($LASTEXITCODE -ne 0) { throw 'Production success regression failed.' }
Write-Output "Fixtures and production completion screenshots saved to $output"
