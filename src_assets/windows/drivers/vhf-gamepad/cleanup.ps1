param(
    [switch] $InstallerBestEffort,
    [ValidateSet(0, 1)]
    [int] $RemoveDriverStorePackage = 0
)

$ErrorActionPreference = 'Stop'

$ownedRootPrefix = 'ROOT\VIBESHINEVIRTUALGAMEPAD\'
$rebootExitCode = 3010

function Write-DriverMessage {
    param([Parameter(Mandatory = $true)][string] $Message)
    Write-Host "[VibeshineVhfGamepad] $Message"
}

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script from an elevated Administrator session because it changes PnP devices.'
    }
}

function Get-OwnedRootDeviceInstanceIds {
    # The PnP instance ID for a root-enumerated source device begins with the
    # exact product hardware ID. Do not enumerate by class or VID/PID: those
    # could reach controllers owned by another application.
    $devices = @(Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction Stop)
    foreach ($device in $devices) {
        $instanceId = [string] $device.PNPDeviceID
        if (-not [string]::IsNullOrWhiteSpace($instanceId) -and
            $instanceId.StartsWith($ownedRootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            $instanceId
        }
    }
}

function Remove-OwnedRootDevices {
    $pnputil = Join-Path $env:WINDIR 'System32\pnputil.exe'
    if (-not (Test-Path -LiteralPath $pnputil -PathType Leaf)) {
        throw "PnPUtil is unavailable: $pnputil"
    }

    $rebootRequired = $false
    $instances = @(Get-OwnedRootDeviceInstanceIds)
    foreach ($instanceId in $instances) {
        Write-DriverMessage "Removing owned source device $instanceId."
        & $pnputil /remove-device $instanceId | Out-Host
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0 -and $exitCode -ne $rebootExitCode) {
            throw "PnPUtil could not remove $instanceId (exit code $exitCode)."
        }
        $rebootRequired = $rebootRequired -or ($exitCode -eq $rebootExitCode)
    }

    if ($instances.Count -eq 0) {
        Write-DriverMessage 'No Vibeshine VHF source devices were present.'
    } else {
        Write-DriverMessage 'Removed only Vibeshine-owned VHF source devices.'
    }
    return $rebootRequired
}

function Remove-OwnedDriverStorePackages {
    $command = Get-Command Get-WindowsDriver -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw 'Get-WindowsDriver is unavailable; refusing to guess Driver Store package names.'
    }

    $pnputil = Join-Path $env:WINDIR 'System32\pnputil.exe'
    if (-not (Test-Path -LiteralPath $pnputil -PathType Leaf)) {
        throw "PnPUtil is unavailable: $pnputil"
    }

    $rebootRequired = $false
    $packages = @(Get-WindowsDriver -Online -All | Where-Object {
        $_.OriginalFileName -eq 'VibeshineVhfGamepad.inf' -and -not [string]::IsNullOrWhiteSpace($_.PublishedName)
    })
    foreach ($package in $packages) {
        Write-DriverMessage "Removing owned Driver Store package $($package.PublishedName)."
        & $pnputil /delete-driver $package.PublishedName /uninstall | Out-Host
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0 -and $exitCode -ne $rebootExitCode) {
            throw "PnPUtil could not remove $($package.PublishedName) (exit code $exitCode)."
        }
        $rebootRequired = $rebootRequired -or ($exitCode -eq $rebootExitCode)
    }
    return $rebootRequired
}

try {
    Assert-Administrator
    $rebootRequired = Remove-OwnedRootDevices
    if ($RemoveDriverStorePackage -ne 0) {
        $rebootRequired = (Remove-OwnedDriverStorePackages) -or $rebootRequired
    }
    if ($rebootRequired) {
        Write-DriverMessage 'VIRTUAL_GAMEPAD_RESTART_REQUIRED'
        exit $rebootExitCode
    }
    exit 0
} catch {
    $message = $_.Exception.Message
    if ($InstallerBestEffort) {
        Write-DriverMessage "VIRTUAL_GAMEPAD_DRIVER_WARNING: $message"
        exit 0
    }
    Write-Error "[VibeshineVhfGamepad] $message" -ErrorAction Continue
    exit 1
}
