<#
    Uninstall ViGEm Bus driver using pnputil.
    Previous implementation queried Win32_Product which only lists MSI packages and
    won’t find the PnP driver installed by the ViGEmBus installer, so it did nothing.

    This script enumerates installed driver packages, finds any with Original Name
    matching ViGEmBus.inf, and deletes them with /uninstall /force.
#>

$ErrorActionPreference = 'Stop'

try {
    $pnputil = Join-Path $env:SystemRoot 'System32/pnputil.exe'
    if (-not (Test-Path $pnputil)) {
        Write-Error "pnputil.exe not found; cannot uninstall ViGEm Bus"
    }

    # Enumerate installed drivers
    $output = & $pnputil /enum-drivers 2>$null

    $entries = @()
    $current = @{}
    foreach ($line in $output) {
        if ($line -match '^\s*Published Name\s*:\s*(\S+)') {
            $current['PublishedName'] = $Matches[1]
        } elseif ($line -match '^\s*Original Name\s*:\s*(.+)$') {
            $current['OriginalName'] = $Matches[1].Trim()
        } elseif ($line -match '^\s*$') {
            if ($current.ContainsKey('PublishedName')) {
                $entries += [pscustomobject]$current
            }
            $current = @{}
        }
    }
    if ($current.ContainsKey('PublishedName')) {
        $entries += [pscustomobject]$current
    }

    $vigemDrivers = $entries | Where-Object { $_.OriginalName -match 'ViGEmBus' }

    if (-not $vigemDrivers -or $vigemDrivers.Count -eq 0) {
        # Fallback: try deleting by INF name directly
        Write-Information "ViGEm Bus driver package not enumerated; trying direct INF uninstall"
        & $pnputil /delete-driver ViGEmBus.inf /uninstall /force
        if ($LASTEXITCODE -eq 0) {
            Write-Information "ViGEm Bus driver uninstalled successfully via INF name"
            exit 0
        }
        Write-Information "ViGEm Bus driver package not found (nothing to uninstall)"
        exit 0
    }

    $hadFailure = $false
    foreach ($drv in $vigemDrivers) {
        Write-Information ("Removing driver package {0} (Original: {1})" -f $drv.PublishedName, $drv.OriginalName)
        & $pnputil /delete-driver $drv.PublishedName /uninstall /force
        if ($LASTEXITCODE -ne 0) {
            Write-Warning ("Failed to remove driver package {0} (exit {1})" -f $drv.PublishedName, $LASTEXITCODE)
            $hadFailure = $true
        }
    }

    if ($hadFailure) {
        exit 1
    } else {
        Write-Information "ViGEm Bus driver uninstalled successfully"
        exit 0
    }
}
catch {
    Write-Warning ("Error during ViGEm Bus uninstall: {0}" -f $_)
    exit 1
}
