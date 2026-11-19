param(
    [int]$InstallDisplayDrivers = 1, [int]$InstallSudovda = 1,
    [int]$InstallVirtualDisplay = 1, [int]$InstallGamepad = 1,
    [int]$DailyUnsignedPolicy = 0, [int]$AllowLocalTestCertificate = 0,
    [int]$AllowLocalGamepadCertificateTrust = 0, [string]$ConsentCertificateThumbprint = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'driver-inventory.ps1')

# Isolated runspaces preserve each standalone script's exit/trap behavior while
# sharing the native, process-owned snapshot. No repeated PowerShell startup or
# disk-cache authentication is required. A new MSI run starts with a fresh cache.
$steps = @(
    @{ Name='sudovda'; Prefix='[SudoVDA]'; Enabled=($InstallDisplayDrivers -ne 0 -and $InstallSudovda -ne 0); Parameters=@{ InstallerBestEffort=$true }; Marker='VIRTUAL_DISPLAY_DRIVER_WARNING' },
    @{ Name='sunshine'; Prefix='[SunshineVirtualDisplay]'; Enabled=($InstallDisplayDrivers -ne 0 -and $InstallVirtualDisplay -ne 0); Parameters=@{ InstallerBestEffort=$true; DailyUnsignedPolicy=$DailyUnsignedPolicy }; Marker='VIRTUAL_DISPLAY_DRIVER_WARNING' },
    @{ Name='vhf-gamepad'; Prefix='[VibeshineVhfGamepad]'; Enabled=($InstallGamepad -ne 0); Parameters=@{ InstallerBestEffort=$true; DailyUnsignedPolicy=$DailyUnsignedPolicy; AllowLocalTestCertificate=$AllowLocalTestCertificate; AllowLocalGamepadCertificateTrust=$AllowLocalGamepadCertificateTrust; ConsentCertificateThumbprint=$ConsentCertificateThumbprint }; Marker='VIRTUAL_GAMEPAD_DRIVER_WARNING' }
)
foreach ($step in $steps) {
    if (-not $step.Enabled) { continue }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $generation = [VibeshineDriverInventory]::Invalidations
    $runner = [powershell]::Create()
    $failureMessage = $null
    $output = @()
    try {
        $null = $runner.AddCommand((Join-Path $PSScriptRoot ($step.Name + '/install.ps1'))).AddParameters($step.Parameters)
        $output = $runner.Invoke()
        $childExitCode = $runner.Runspace.SessionStateProxy.GetVariable('LASTEXITCODE')
        if ($runner.HadErrors -or $runner.InvocationStateInfo.State -eq 'Failed' -or ($null -ne $childExitCode -and $childExitCode -notin @(0,3010))) {
            $failureMessage = "exit=$childExitCode $($runner.Streams.Error -join '; ')"
        }
    } catch {
        $failureMessage = $_.Exception.Message
    } finally {
        foreach ($line in $output) { Write-Host $line }
        foreach ($line in $runner.Streams.Information) { Write-Host $line.MessageData.ToString() }
        foreach ($line in $runner.Streams.Warning) { Write-Warning $line.Message }
        if ($failureMessage) {
            Write-Warning "$($step.Prefix) $($step.Marker): $($step.Name) setup failed: $failureMessage"
        }
        $runner.Dispose()
        if ([VibeshineDriverInventory]::Invalidations -ne $generation) {
            try { $null = [VibeshineDriverInventory]::Get() } catch {
                Write-Warning "$($step.Prefix) $($step.Marker): Post-change inventory failed; further decisions must use a fresh snapshot. $($_.Exception.Message)"
            }
        }
        Write-Host "DRIVER_OPERATION:$($step.Name) elapsed_ms=$($clock.ElapsedMilliseconds)"
    }
}
Write-Host ([VibeshineDriverInventory]::Summary())
