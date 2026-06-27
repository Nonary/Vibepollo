param(
    [string]$InstallVirtualDisplayDriver = ''
)

$ErrorActionPreference = 'Stop'

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
        Set-Content -LiteralPath $ConfigPath -Value ($line + [Environment]::NewLine) -NoNewline -Encoding UTF8
        return $true
    }

    $original = Get-Content -LiteralPath $ConfigPath -Raw -ErrorAction Stop
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

    Set-Content -LiteralPath $ConfigPath -Value $updated -NoNewline -Encoding UTF8
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
    if ($null -eq $enabled -or -not $enabled) {
        return $false
    }

    $configPath = Join-Path $RootDir 'config\sunshine.conf'
    return Set-SunshineConfigOption `
        -ConfigPath $configPath `
        -Name 'dd_use_sunshine_virtual_display_driver' `
        -Value 'enabled'
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

    $original = Get-Content -LiteralPath $ConfigPath -Raw -ErrorAction Stop
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

    Set-Content -LiteralPath $ConfigPath -Value $updated -NoNewline -Encoding UTF8
    return $true
}

function Convert-SplitFrameEncodingJsonNode {
    param(
        [AllowNull()]
        [object]$Node,
        [ref]$Changed
    )

    if ($null -eq $Node) {
        return $null
    }

    if ($Node -is [System.Management.Automation.PSCustomObject]) {
        $result = [ordered]@{}
        foreach ($property in $Node.PSObject.Properties) {
            $isLegacySplitEncodeProperty = $property.Name -eq 'nvenc_force_split_encode'
            $targetName = if ($isLegacySplitEncodeProperty) {
                $Changed.Value = $true
                'nvenc_split_encode'
            } else {
                $property.Name
            }

            $value = Convert-SplitFrameEncodingJsonNode -Node $property.Value -Changed $Changed
            if ($targetName -eq 'nvenc_split_encode') {
                $converted = Convert-LegacySplitEncodeValue $value
                if (($converted -is [string]) -or ($converted -ne $value)) {
                    if (-not (($converted -is [string]) -and ($value -is [string]) -and $converted -ceq $value)) {
                        $Changed.Value = $true
                    }
                }
                $value = $converted
            }

            if (-not $result.Contains($targetName)) {
                $result[$targetName] = $value
            } elseif ($targetName -eq 'nvenc_split_encode' -and -not $isLegacySplitEncodeProperty) {
                $result[$targetName] = $value
                $Changed.Value = $true
            }
        }
        return [pscustomobject]$result
    }

    if ($Node -is [System.Collections.IEnumerable] -and $Node -isnot [string]) {
        $result = @()
        foreach ($item in $Node) {
            $result += ,(Convert-SplitFrameEncodingJsonNode -Node $item -Changed $Changed)
        }
        return $result
    }

    return $Node
}

function Update-SplitFrameEncodingInJson {
    param(
        [Parameter(Mandatory = $true)]
        [string]$JsonPath
    )

    if (-not (Test-Path -LiteralPath $JsonPath)) {
        return $false
    }

    $original = Get-Content -LiteralPath $JsonPath -Raw -ErrorAction Stop
    if ([string]::IsNullOrWhiteSpace($original)) {
        return $false
    }

    try {
        $parsed = $original | ConvertFrom-Json
    } catch {
        return $false
    }

    $changed = $false
    $updated = Convert-SplitFrameEncodingJsonNode -Node $parsed -Changed ([ref]$changed)
    if (-not $changed) {
        return $false
    }

    $serialized = $updated | ConvertTo-Json -Depth 100
    Set-Content -LiteralPath $JsonPath -Value $serialized -NoNewline -Encoding UTF8
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
