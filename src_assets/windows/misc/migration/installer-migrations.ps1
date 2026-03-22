param()

$ErrorActionPreference = 'Stop'

function Update-SplitFrameEncodingSetting {
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
        '(?im)^(\s*nvenc_force_split_encode\s*=\s*)(true|false|yes|no|on|off|enable|disable|enabled|disabled|1|0)(\s*(?:[#;].*)?)$',
        {
            param($match)

            $rawValue = $match.Groups[2].Value.Trim().ToLowerInvariant()
            $mappedValue = switch ($rawValue) {
                { $_ -in @('true', 'yes', 'on', 'enable', 'enabled', '1') } { 'enabled'; break }
                { $_ -in @('false', 'no', 'off', 'disable', 'disabled', '0') } { 'disabled'; break }
                default { $null }
            }

            if ([string]::IsNullOrEmpty($mappedValue)) {
                return $match.Value
            }

            return '{0}{1}{2}' -f $match.Groups[1].Value, $mappedValue, $match.Groups[3].Value
        }
    )

    if ($updated -ceq $original) {
        return $false
    }

    Set-Content -LiteralPath $ConfigPath -Value $updated -NoNewline -Encoding UTF8
    return $true
}

$rootDir = Split-Path -Parent $PSScriptRoot
$candidateConfigs = @(
    (Join-Path $rootDir 'config\sunshine.conf'),
    (Join-Path $rootDir 'sunshine.conf')
) | Select-Object -Unique

$changedAny = $false
foreach ($configPath in $candidateConfigs) {
    if (Update-SplitFrameEncodingSetting -ConfigPath $configPath) {
        Write-Output "Migrated nvenc_force_split_encode values in $configPath"
        $changedAny = $true
    }
}

if (-not $changedAny) {
    Write-Output 'No installer config migrations were needed.'
}
