param(
    [Parameter(Mandatory = $true)]
    [string]$InstallRoot
)

$ErrorActionPreference = 'Stop'

function Remove-KnownItem {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BasePath,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if ([string]::IsNullOrWhiteSpace($BasePath) -or [string]::IsNullOrWhiteSpace($Name)) {
        return
    }

    $path = Join-Path -Path $BasePath -ChildPath $Name
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Remove-DirectoryIfEmpty {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        return
    }

    $child = Get-ChildItem -LiteralPath $Path -Force -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $child) {
        Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    }
}

$root = [System.IO.Path]::GetFullPath($InstallRoot)
$config = Join-Path -Path $root -ChildPath 'config'

# Factory reset removes only known Vibepollo data. It intentionally does not
# recurse through the install root or config directory looking for arbitrary
# files, so user-created files added after install are preserved.
$knownConfigItems = @(
    'apps.json',
    'sunshine.conf',
    'sunshine.log',
    'sunshine_state.json',
    'vibeshine_state.json',
    'virtual_display_cache.json',
    'nvprefs_undo.json',
    'sunshine_playnite.log',
    'credentials',
    'covers',
    'logs'
)

foreach ($item in $knownConfigItems) {
    Remove-KnownItem -BasePath $config -Name $item
}

# Older Sunshine/Vibepollo versions could keep app data directly under the
# install root. Remove only those known legacy locations during factory reset.
foreach ($item in $knownConfigItems) {
    Remove-KnownItem -BasePath $root -Name $item
}

Remove-DirectoryIfEmpty -Path $config
