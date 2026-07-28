@echo off

rem Get sunshine root directory
for %%I in ("%~dp0\..") do set "OLD_DIR=%%~fI"

rem Create the config directory if it didn't already exist
set "NEW_DIR=%OLD_DIR%\config"
if not exist "%NEW_DIR%\" mkdir "%NEW_DIR%"
icacls "%NEW_DIR%" /reset

rem Migrate all files that aren't already present in the config dir
if exist "%OLD_DIR%\apps.json" (
    if not exist "%NEW_DIR%\apps.json" (
        move "%OLD_DIR%\apps.json" "%NEW_DIR%\apps.json"
        icacls "%NEW_DIR%\apps.json" /reset
    )
)
if exist "%OLD_DIR%\sunshine.conf" (
    if not exist "%NEW_DIR%\sunshine.conf" (
        move "%OLD_DIR%\sunshine.conf" "%NEW_DIR%\sunshine.conf"
        icacls "%NEW_DIR%\sunshine.conf" /reset
    )
)
if exist "%OLD_DIR%\sunshine_state.json" (
    if not exist "%NEW_DIR%\sunshine_state.json" (
        move "%OLD_DIR%\sunshine_state.json" "%NEW_DIR%\sunshine_state.json"
        icacls "%NEW_DIR%\sunshine_state.json" /reset
    )
)

rem Migrate the credentials directory
if exist "%OLD_DIR%\credentials\" (
    if not exist "%NEW_DIR%\credentials\" (
        move "%OLD_DIR%\credentials" "%NEW_DIR%\"
    )
)

rem Create the credentials directory if it wasn't migrated or already existing
if not exist "%NEW_DIR%\credentials\" mkdir "%NEW_DIR%\credentials"

rem Disallow read access to the credentials directory contents for normal users
rem Note: We must use the SIDs directly because "Users" and "Administrators" are localized
icacls "%NEW_DIR%\credentials" /inheritance:r
icacls "%NEW_DIR%\credentials" /grant:r *S-1-5-18:(OI)(CI)(F)
icacls "%NEW_DIR%\credentials" /grant:r *S-1-5-32-544:(OI)(CI)(F)
icacls "%NEW_DIR%\credentials" /grant:r *S-1-5-32-545:(R)

rem Migrate the covers directory
if exist "%OLD_DIR%\covers\" (
    if not exist "%NEW_DIR%\covers\" (
        move "%OLD_DIR%\covers" "%NEW_DIR%\"

        rem Fix apps.json image path values that point at the old covers directory
        rem Preserve UTF-8 application names; Windows PowerShell otherwise reads UTF-8 without a BOM as ANSI.
        rem Only rewrite a file that exists and was read; writing an unread value would leave a
        rem zero-byte apps.json that hides the legacy file from every later migration.
        "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -c "$appsPath = '%NEW_DIR%\apps.json'; if (Test-Path -LiteralPath $appsPath) { $utf8 = New-Object System.Text.UTF8Encoding($false); $appsJson = [System.IO.File]::ReadAllText($appsPath, $utf8); if ($null -ne $appsJson) { $updatedAppsJson = $appsJson.Replace('.\/covers\/', '.\/config\/covers\/'); [System.IO.File]::WriteAllText($appsPath, $updatedAppsJson, $utf8) } }"
    )
)

rem Remove log files
del "%OLD_DIR%\*.txt"
del "%OLD_DIR%\*.log"
