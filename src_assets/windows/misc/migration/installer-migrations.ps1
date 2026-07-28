param(
    [string]$InstallVirtualDisplayDriver = ''
)

$ErrorActionPreference = 'Stop'

# Windows PowerShell 5.1 treats UTF-8 files without a BOM as ANSI when using
# Get-Content. Configuration and JSON files are UTF-8, so use explicit .NET
# APIs for every upgrade migration read/write and keep the result BOM-free.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Read-Utf8TextFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return [System.IO.File]::ReadAllText($Path, $utf8NoBom)
}

function Write-Utf8TextFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [AllowEmptyString()]
        [string]$Value
    )

    [System.IO.File]::WriteAllText($Path, $Value, $utf8NoBom)
}

function Convert-InstallerBooleanValue {
    param(
        [AllowNull()]
        [string]$Value
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $null
    }

    switch ($Value.Trim().ToLowerInvariant()) {
        { $_ -in @('1', 'true', 'yes', 'on', 'enable', 'enabled') } { return $true }
        { $_ -in @('0', 'false', 'no', 'off', 'disable', 'disabled') } { return $false }
        default { return $null }
    }
}

function Set-SunshineConfigOption {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ConfigPath,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    $configDir = Split-Path -Parent $ConfigPath
    if (-not (Test-Path -LiteralPath $configDir)) {
        New-Item -ItemType Directory -Force -Path $configDir | Out-Null
    }

    $line = '{0} = {1}' -f $Name, $Value
    if (-not (Test-Path -LiteralPath $ConfigPath)) {
        Write-Utf8TextFile -Path $ConfigPath -Value ($line + [Environment]::NewLine)
        return $true
    }

    $original = Read-Utf8TextFile -Path $ConfigPath
    $pattern = '(?im)^(\s*)' + [System.Text.RegularExpressions.Regex]::Escape($Name) + '(\s*=\s*)([^#;\r\n]*)(\s*(?:[#;].*)?)$'
    $updated = [System.Text.RegularExpressions.Regex]::Replace(
        $original,
        $pattern,
        {
            param($match)

            return '{0}{1}{2}{3}{4}' -f `
                $match.Groups[1].Value, `
                $Name, `
                $match.Groups[2].Value, `
                $Value, `
                $match.Groups[4].Value
        },
        1
    )

    if ($updated -ceq $original) {
        $separator = if ($original.EndsWith("`n") -or $original.EndsWith("`r")) { '' } else { [Environment]::NewLine }
        $updated = $original + $separator + $line + [Environment]::NewLine
    }

    if ($updated -ceq $original) {
        return $false
    }

    Write-Utf8TextFile -Path $ConfigPath -Value $updated
    return $true
}

function Update-SunshineVirtualDriverPreference {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RootDir,

        [AllowNull()]
        [string]$InstallVirtualDisplayDriver
    )

    $enabled = Convert-InstallerBooleanValue -Value $InstallVirtualDisplayDriver
    if ($null -eq $enabled) {
        return $false
    }

    $configPath = Join-Path $RootDir 'config\sunshine.conf'
    return Set-SunshineConfigOption `
        -ConfigPath $configPath `
        -Name 'dd_use_sunshine_virtual_display_driver' `
        -Value $(if ($enabled) { 'enabled' } else { 'disabled' })
}

function Convert-LegacySplitEncodeValue {
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Value
    )

    if ($null -eq $Value) {
        return $Value
    }

    if ($Value -is [bool]) {
        return $(if ($Value) { 'enabled' } else { 'disabled' })
    }

    if ($Value -is [byte] -or $Value -is [int16] -or $Value -is [int32] -or $Value -is [int64]) {
        if ([int64]$Value -eq 1) {
            return 'enabled'
        }
        if ([int64]$Value -eq 0) {
            return 'disabled'
        }
        return $Value
    }

    if ($Value -isnot [string]) {
        return $Value
    }

    $rawValue = $Value.Trim().ToLowerInvariant()
    switch ($rawValue) {
        { $_ -in @('true', 'yes', 'on', 'enable', 'enabled', '1') } { return 'enabled' }
        { $_ -in @('false', 'no', 'off', 'disable', 'disabled', '0') } { return 'disabled' }
        default { return $Value.Trim() }
    }
}

function Update-SplitFrameEncodingInConfig {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ConfigPath
    )

    if (-not (Test-Path -LiteralPath $ConfigPath)) {
        return $false
    }

    $original = Read-Utf8TextFile -Path $ConfigPath
    if ([string]::IsNullOrWhiteSpace($original)) {
        return $false
    }

    $updated = [System.Text.RegularExpressions.Regex]::Replace(
        $original,
        '(?im)^(\s*)nvenc_force_split_encode(\s*=\s*)([^#;\r\n]+?)(\s*(?:[#;].*)?)$',
        {
            param($match)

            $convertedValue = Convert-LegacySplitEncodeValue $match.Groups[3].Value
            return '{0}nvenc_split_encode{1}{2}{3}' -f `
                $match.Groups[1].Value, `
                $match.Groups[2].Value, `
                $convertedValue, `
                $match.Groups[4].Value
        }
    )

    if ($updated -ceq $original) {
        return $false
    }

    Write-Utf8TextFile -Path $ConfigPath -Value $updated
    return $true
}

# Normalizes one raw JSON scalar token for the split encode option. Tokens that
# do not map to a known legacy value are returned verbatim, so the migration
# never rewrites user data it does not understand.
function Convert-LegacySplitEncodeJsonToken {
    param(
        [AllowEmptyString()]
        [string]$Token
    )

    if ([string]::IsNullOrEmpty($Token)) {
        return $Token
    }

    try {
        $decoded = ('{"value":' + $Token + '}' | ConvertFrom-Json).value
    } catch {
        return $Token
    }

    if ($null -eq $decoded) {
        return $Token
    }

    $converted = Convert-LegacySplitEncodeValue $decoded
    if ($converted -is [string]) {
        if ($converted -ceq 'enabled') {
            return '"enabled"'
        }
        if ($converted -ceq 'disabled') {
            return '"disabled"'
        }
    }

    return $Token
}

# Read-only probe that reports whether any object already carries both the legacy
# and the modern key. Renaming in place would emit a duplicate JSON key there, and
# the modern value is the one that already wins, so such files are left untouched.
function Test-SplitFrameEncodingKeyCollision {
    param(
        [AllowNull()]
        [object]$Node
    )

    if ($null -eq $Node) {
        return $false
    }

    if ($Node -is [System.Management.Automation.PSCustomObject]) {
        $names = @($Node.PSObject.Properties | ForEach-Object { $_.Name })
        if (($names -ccontains 'nvenc_force_split_encode') -and ($names -ccontains 'nvenc_split_encode')) {
            return $true
        }

        foreach ($property in $Node.PSObject.Properties) {
            if (Test-SplitFrameEncodingKeyCollision -Node $property.Value) {
                return $true
            }
        }

        return $false
    }

    if ($Node -is [System.Collections.IEnumerable] -and $Node -isnot [string]) {
        foreach ($item in $Node) {
            if (Test-SplitFrameEncodingKeyCollision -Node $item) {
                return $true
            }
        }

        return $false
    }

    return $false
}

function Update-SplitFrameEncodingInJson {
    param(
        [Parameter(Mandatory = $true)]
        [string]$JsonPath
    )

    if (-not (Test-Path -LiteralPath $JsonPath)) {
        return $false
    }

    $original = Read-Utf8TextFile -Path $JsonPath
    if ([string]::IsNullOrWhiteSpace($original)) {
        return $false
    }

    # Never rebuild these documents from the PowerShell object model. Windows
    # PowerShell 5.1 cannot round-trip JSON: ConvertTo-Json has no -AsArray, and a
    # function that returns an array has that array unrolled into the pipeline, so
    # single element arrays come back as scalars and empty arrays come back as
    # $null. In apps.json that turns "apps" and "prep-cmd" into objects, which
    # process.cpp rejects with its is_array() guards, and its v1->v2 migration then
    # writes the emptied app list back to disk. Rewrite only the legacy key in the
    # raw text instead, exactly like the sunshine.conf migration above, so every
    # other byte of the user's file is preserved.
    $keyPattern = '(?<!\\)"nvenc_force_split_encode"'
    if ($original -cnotmatch ($keyPattern + '\s*:')) {
        return $false
    }

    try {
        $parsed = $original | ConvertFrom-Json
    } catch {
        return $false
    }

    if (Test-SplitFrameEncodingKeyCollision -Node $parsed) {
        return $false
    }

    $jsonScalar = '"(?:[^"\\]|\\.)*"|true|false|null|-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?'
    $updated = [System.Text.RegularExpressions.Regex]::Replace(
        $original,
        ($keyPattern + '(\s*:\s*)(' + $jsonScalar + ')?'),
        {
            param($match)

            return '"nvenc_split_encode"{0}{1}' -f `
                $match.Groups[1].Value, `
                (Convert-LegacySplitEncodeJsonToken -Token $match.Groups[2].Value)
        }
    )

    if ($updated -ceq $original) {
        return $false
    }

    # Fail safe: never write a document that stopped parsing.
    try {
        [void]($updated | ConvertFrom-Json)
    } catch {
        return $false
    }

    Write-Utf8TextFile -Path $JsonPath -Value $updated
    return $true
}

function Invoke-IcaclsBestEffort {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $icacls = Join-Path $env:SystemRoot 'System32\icacls.exe'
    $output = & $icacls @Arguments 2>&1
    $exitCode = $LASTEXITCODE

    if ($output) {
        $output | ForEach-Object { Write-Output $_ }
    }
    if ($exitCode -ne 0) {
        Write-Output "icacls exited with code $exitCode for arguments: $($Arguments -join ' ')"
    }

    return ($exitCode -eq 0)
}

function Repair-ConfigAclInheritance {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RootDir
    )

    $configDir = Join-Path $RootDir 'config'
    if (-not (Test-Path -LiteralPath $configDir)) {
        return
    }

    [void](Invoke-IcaclsBestEffort -Arguments @($configDir, '/inheritance:e'))
    [void](Invoke-IcaclsBestEffort -Arguments @($configDir, '/reset'))

    $configFiles = @(
        (Join-Path $configDir 'sunshine_state.json'),
        (Join-Path $configDir 'vibeshine_state.json'),
        (Join-Path $configDir 'sunshine.conf'),
        (Join-Path $configDir 'apps.json')
    )

    foreach ($path in $configFiles) {
        if (Test-Path -LiteralPath $path) {
            [void](Invoke-IcaclsBestEffort -Arguments @($path, '/inheritance:e'))
            [void](Invoke-IcaclsBestEffort -Arguments @($path, '/reset'))
        }
    }

    # Re-harden the intentionally private certificate/key directory after the
    # shared config reset. Use SIDs so this works on localized Windows builds.
    $credentialsDir = Join-Path $configDir 'credentials'
    if (Test-Path -LiteralPath $credentialsDir) {
        [void](Invoke-IcaclsBestEffort -Arguments @($credentialsDir, '/inheritance:r'))
        [void](Invoke-IcaclsBestEffort -Arguments @($credentialsDir, '/grant:r', '*S-1-5-18:(OI)(CI)(F)'))
        [void](Invoke-IcaclsBestEffort -Arguments @($credentialsDir, '/grant:r', '*S-1-5-32-544:(OI)(CI)(F)'))
        [void](Invoke-IcaclsBestEffort -Arguments @($credentialsDir, '/grant:r', '*S-1-5-32-545:(R)'))
    }
}

$rootDir = Split-Path -Parent $PSScriptRoot

Repair-ConfigAclInheritance -RootDir $rootDir

$candidateConfigs = @(
    (Join-Path $rootDir 'config\sunshine.conf'),
    (Join-Path $rootDir 'sunshine.conf')
) | Select-Object -Unique

$candidateJsonFiles = @(
    (Join-Path $rootDir 'config\apps.json'),
    (Join-Path $rootDir 'apps.json'),
    (Join-Path $rootDir 'config\sunshine_state.json'),
    (Join-Path $rootDir 'sunshine_state.json'),
    (Join-Path $rootDir 'config\vibeshine_state.json'),
    (Join-Path $rootDir 'vibeshine_state.json')
) | Select-Object -Unique

$changedAny = $false
if (Update-SunshineVirtualDriverPreference -RootDir $rootDir -InstallVirtualDisplayDriver $InstallVirtualDisplayDriver) {
    Write-Output 'Updated Vibepollo Display Driver preference from installer selection.'
    $changedAny = $true
}

foreach ($configPath in $candidateConfigs) {
    if (Update-SplitFrameEncodingInConfig -ConfigPath $configPath) {
        Write-Output "Migrated nvenc_force_split_encode to nvenc_split_encode in $configPath"
        $changedAny = $true
    }
}

foreach ($jsonPath in $candidateJsonFiles) {
    if (Update-SplitFrameEncodingInJson -JsonPath $jsonPath) {
        Write-Output "Migrated nvenc_force_split_encode to nvenc_split_encode in $jsonPath"
        $changedAny = $true
    }
}

if (-not $changedAny) {
    Write-Output 'No installer config migrations were needed.'
}
