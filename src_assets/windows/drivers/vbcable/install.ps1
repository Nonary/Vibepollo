# VB-Audio CABLE silent driver installer
# Uses pnputil to install the bundled WDM driver directly — no GUI setup exe needed.
# Elevation is provided by the Vibepollo C++ layer (CreateProcessAsUserW with admin token).
#
# This script is run automatically by Vibepollo when mic passthrough is
# configured and VB-Audio CABLE is not already installed.
$ErrorActionPreference = 'Stop'

$inf = Join-Path $PSScriptRoot 'vbMmeCable64_win7.inf'
if (-not (Test-Path $inf)) {
    throw "Driver INF not found at $inf"
}

Write-Output "Installing VB-Audio CABLE driver via pnputil: $inf"
$proc = Start-Process -FilePath 'pnputil.exe' -ArgumentList '/add-driver', $inf, '/install' -Wait -PassThru -NoNewWindow
if ($proc.ExitCode -notin @(0, 3010)) {
    throw "pnputil exited with code $($proc.ExitCode)"
}

Write-Output "VB-Audio CABLE driver installed successfully (exit code $($proc.ExitCode))."
exit 0
