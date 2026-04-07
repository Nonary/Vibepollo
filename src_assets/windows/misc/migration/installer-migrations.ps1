param()

$ErrorActionPreference = 'Stop'

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
            $targetName = if ($property.Name -eq 'nvenc_force_split_encode') {
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

$rootDir = Split-Path -Parent $PSScriptRoot
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
