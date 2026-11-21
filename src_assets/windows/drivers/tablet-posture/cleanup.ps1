<#
.SYNOPSIS
  Removes the laptop/slate indicator device the Vibepollo host creates for tablet posture.

.DESCRIPTION
  The host creates one root-enumerated device (ROOT\HIDCLASS\*, hardware id PNP0C60) bound to Microsoft's
  inbox "GPIO Laptop or Slate Indicator Driver". This script removes only such root-enumerated nodes.
  A real indicator that firmware exposes (ACPI\PNP0C60) is never touched. The inbox driver package stays
  in Windows; nothing else is installed.
#>
[CmdletBinding()]
param([switch]$InstallerBestEffort)

$ErrorActionPreference = 'Continue'
try {
    $nodes = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
            $_.InstanceId -like 'ROOT\HIDCLASS\*' -and ($_.HardwareID -contains 'PNP0C60')
        })
    foreach ($node in $nodes) {
        Write-Host "Removing laptop/slate indicator $($node.InstanceId)"
        & "$env:SystemRoot\System32\pnputil.exe" /remove-device $node.InstanceId | Out-Null
    }
} catch {
    Write-Warning "Laptop/slate indicator cleanup failed: $($_.Exception.Message)"
    if (-not $InstallerBestEffort) { throw }
}
exit 0
