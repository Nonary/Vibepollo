param([string]$RepositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..')))
$ErrorActionPreference = 'Stop'
$savedExitCode = $global:LASTEXITCODE
function Check([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
. (Join-Path $RepositoryRoot 'src_assets/windows/drivers/driver-inventory.ps1')
$initial = [VibeshineDriverInventory]::Get()
foreach ($pair in @(
    @('SudoVDA.inf', 'ROOT\SUDOMAKER\SUDOVDA'),
    @('SunshineVirtualDisplayDriver.inf', 'ROOT\SunshineVirtualDisplay'),
    @('VibeshineVhfGamepad.inf', 'ROOT\VIBESHINEVIRTUALGAMEPAD'))) {
    $null = Get-SharedDriverInventory -InfName $pair[0] -HardwareId $pair[1]
}
Check ([VibeshineDriverInventory]::PackageQueries -eq 1 -and [VibeshineDriverInventory]::DeviceQueries -eq 1) 'Three components must share one complete inventory'
$other = [powershell]::Create()
try {
    $null = $other.AddScript('[VibeshineDriverInventory]::Get()')
    $snapshot = @($other.Invoke())[0].BaseObject
    # PSObject adaptation differs across PowerShell versions; query counters
    # and a reference returned through the wrapper both exercise real reuse.
    Check (-not $other.HadErrors) 'A separate runspace must see the shared provider'
    Check ([VibeshineDriverInventory]::PackageQueries -eq 1) 'A second runspace repeated package discovery'
} finally { $other.Dispose() }

# Exercise the production mutation boundary with a failing external command.
# No OS mutation is performed: a function stands in for pnputil.
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $RepositoryRoot 'src_assets/windows/drivers/vhf-gamepad/install.ps1'),[ref]$tokens,[ref]$errors)
foreach ($name in @('Invoke-PnpUtil')) {
    $definition=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name}.GetNewClosure(),$true)
    . ([scriptblock]::Create($definition.Extent.Text))
}
function Resolve-SystemToolPath { param($ToolName) return 'Invoke-FailedFixtureTool' }
function Assert-File { param($Path) }
function Invoke-FailedFixtureTool { $global:LASTEXITCODE=1 }
$failed=$false
try { $null=Invoke-PnpUtil -Arguments @('/add-driver','fixture.inf') } catch { $failed=$true }
Check $failed 'A failed driver mutation must remain a failure'
$null=[VibeshineDriverInventory]::Get()
Check ([VibeshineDriverInventory]::PackageQueries -eq 2 -and [VibeshineDriverInventory]::DeviceQueries -eq 2) 'Even failed/partial staging must invalidate the shared snapshot'

# Run the real coordinator against harmless standalone fixture scripts. This
# covers child exit/throw isolation, opt-out, and sharing across components.
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('shared-driver-test-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $scratch
try {
    foreach ($file in @('driver-inventory.ps1','install-bundled-drivers.ps1')) {
        Copy-Item -LiteralPath (Join-Path $RepositoryRoot "src_assets/windows/drivers/$file") -Destination $scratch
    }
    foreach ($name in @('sudovda','sunshine','vhf-gamepad')) {
        $null=New-Item -ItemType Directory -Path (Join-Path $scratch $name)
        $body='param([switch]$InstallerBestEffort,[int]$DailyUnsignedPolicy,[int]$AllowLocalTestCertificate,[int]$AllowLocalGamepadCertificateTrust,[string]$ConsentCertificateThumbprint)' + "`n" +
            '$null=[VibeshineDriverInventory]::Get(); Write-Host "FIXTURE_STEP:'+$name+'"; '
        $body += if ($name -eq 'sunshine') { 'throw "fixture failure"' } else { 'exit 0' }
        [IO.File]::WriteAllText((Join-Path $scratch "$name/install.ps1"),$body)
    }
    $all=(& (Join-Path $scratch 'install-bundled-drivers.ps1') 6>&1 3>&1 | Out-String -Width 32768)
    Check ($all -match 'FIXTURE_STEP:sudovda' -and $all -match 'FIXTURE_STEP:sunshine' -and $all -match 'FIXTURE_STEP:vhf-gamepad') "An optional driver failure must not prevent subsequent components: $all"
    Check ($all -match 'VIRTUAL_DISPLAY_DRIVER_WARNING' -and $all -match 'fixture\s+failure') "Coordinator lost the component failure diagnostic: $all"
    [IO.File]::WriteAllText((Join-Path $scratch 'sunshine/install.ps1'), 'param([switch]$InstallerBestEffort,[int]$DailyUnsignedPolicy); exit 1')
    $nonzero=(& (Join-Path $scratch 'install-bundled-drivers.ps1') -InstallSudovda 0 6>&1 3>&1 | Out-String -Width 32768)
    Check ($nonzero -match 'VIRTUAL_DISPLAY_DRIVER_WARNING' -and $nonzero -match 'exit=1' -and $nonzero -match 'FIXTURE_STEP:vhf-gamepad') 'A nonzero standalone-script exit must report failure and continue'
    $disabled=(& (Join-Path $scratch 'install-bundled-drivers.ps1') -InstallDisplayDrivers 0 -InstallGamepad 0 6>&1 3>&1 | Out-String -Width 32768)
    Check ($disabled -notmatch 'FIXTURE_STEP:') 'Explicit opt-out must run no driver scripts'
    $gamepadOnly=(& (Join-Path $scratch 'install-bundled-drivers.ps1') -InstallDisplayDrivers 0 6>&1 3>&1 | Out-String -Width 32768)
    Check ($gamepadOnly -match 'FIXTURE_STEP:vhf-gamepad' -and $gamepadOnly -notmatch 'sunshine|sudovda') 'Display opt-out must still permit a selected base gamepad update'
    Write-Output ([VibeshineDriverInventory]::Summary())
    Write-Output 'Shared inventory, cross-runspace reuse, failed-mutation invalidation, component isolation and selection tests passed. No drivers/trust/boot state changed.'
} finally {
    $resolved=[IO.Path]::GetFullPath($scratch)
    $temp=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($temp,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^shared-driver-test-[0-9a-f]{32}$') { throw 'Unsafe test cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
    $global:LASTEXITCODE = $savedExitCode
}
