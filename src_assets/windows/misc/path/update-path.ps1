param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("add", "remove")]
    [string] $Action
)

$ErrorActionPreference = "Stop"

$registrySubKey = "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"
$valueName = "Path"

# Use the Registry API instead of reg.exe. Existing PATH values may contain
# malformed or unmatched quotes; passing those values through cmd/reg.exe can
# turn a recoverable PATH edit into an installer-fatal syntax error.

function Get-NormalizedPathEntry {
    param([AllowNull()][string] $Entry)

    if ([string]::IsNullOrWhiteSpace($Entry)) {
        return ""
    }

    $normalized = $Entry.Trim().Trim('"').Trim()

    while ($normalized.Length -gt 3 -and ($normalized.EndsWith("\") -or $normalized.EndsWith("/"))) {
        $normalized = $normalized.Substring(0, $normalized.Length - 1)
    }

    return $normalized
}

function Test-PathEntryEqual {
    param(
        [AllowNull()][string] $Left,
        [AllowNull()][string] $Right
    )

    $normalizedLeft = Get-NormalizedPathEntry -Entry $Left
    $normalizedRight = Get-NormalizedPathEntry -Entry $Right

    return [string]::Equals($normalizedLeft, $normalizedRight, [System.StringComparison]::OrdinalIgnoreCase)
}

function Join-PathEntries {
    param([AllowNull()][string[]] $Entries)

    if ($null -eq $Entries) {
        return ""
    }

    return ($Entries -join ";")
}

$rootDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..")).TrimEnd("\", "/")
$pathsToManage = @(
    $rootDir,
    (Join-Path $rootDir "tools")
)

Write-Output "Apollo root directory: $rootDir"

$key = $null
try {
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($registrySubKey, $true)
    if ($null -eq $key) {
        throw "Could not open HKLM:\$registrySubKey"
    }

    $currentPath = [string] $key.GetValue($valueName, "", [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    Write-Output "Current path: $currentPath"

    $entries = if ([string]::IsNullOrEmpty($currentPath)) {
        @()
    } else {
        @($currentPath -split ";")
    }

    if ($Action -eq "add") {
        $changed = $false

        foreach ($pathToAdd in $pathsToManage) {
            $alreadyPresent = $false

            foreach ($entry in $entries) {
                if (Test-PathEntryEqual -Left $entry -Right $pathToAdd) {
                    $alreadyPresent = $true
                    break
                }
            }

            if ($alreadyPresent) {
                Write-Output "$pathToAdd already in path"
            } else {
                Write-Output "Adding to path: $pathToAdd"
                $entries += $pathToAdd
                $changed = $true
            }
        }

        if ($changed) {
            $newPath = Join-PathEntries -Entries $entries
            $key.SetValue($valueName, $newPath, [Microsoft.Win32.RegistryValueKind]::ExpandString)
            Write-Output "Successfully added Apollo directories to PATH"
        } else {
            Write-Output "No changes needed to PATH"
        }

        exit 0
    }

    if ($Action -eq "remove") {
        $keptEntries = [System.Collections.Generic.List[string]]::new()
        $removedEntries = [System.Collections.Generic.List[string]]::new()

        foreach ($entry in $entries) {
            $shouldRemove = $false

            foreach ($pathToRemove in $pathsToManage) {
                if (Test-PathEntryEqual -Left $entry -Right $pathToRemove) {
                    $shouldRemove = $true
                    break
                }
            }

            if ($shouldRemove) {
                $removedEntries.Add($entry)
            } else {
                $keptEntries.Add($entry)
            }
        }

        if ($removedEntries.Count -gt 0) {
            $newPath = Join-PathEntries -Entries $keptEntries.ToArray()
            $key.SetValue($valueName, $newPath, [Microsoft.Win32.RegistryValueKind]::ExpandString)

            foreach ($entry in $removedEntries) {
                Write-Output "Removing from path: $entry"
            }

            Write-Output "Successfully removed Apollo directories from PATH"
        } else {
            foreach ($pathToRemove in $pathsToManage) {
                Write-Output "$pathToRemove not found in path"
            }
            Write-Output "No changes needed to PATH"
        }

        exit 0
    }
} catch {
    if ($Action -eq "add") {
        Write-Output "Failed to add Apollo directories to PATH"
    } else {
        Write-Output "Failed to remove Apollo directories from PATH"
    }
    Write-Output $_.Exception.Message
    exit 1
} finally {
    if ($null -ne $key) {
        $key.Close()
    }
}
