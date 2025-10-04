@echo off
setlocal EnableDelayedExpansion

rem Check if parameter is provided
if "%~1"=="" (
    echo Usage: %0 [add^|remove]
    echo   add    - Adds Apollo directories to system PATH
    echo   remove - Removes Apollo directories from system PATH
    exit /b 1
)

rem Get Apollo root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"
echo Apollo root directory: !ROOT_DIR!

rem Define directories to add to path
set "PATHS_TO_MANAGE[0]=!ROOT_DIR!"
set "PATHS_TO_MANAGE[1]=!ROOT_DIR!\tools"

rem System path registry location
set "KEY_NAME=HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager\Environment"
set "VALUE_NAME=Path"

rem Get the current path
for /f "tokens=2*" %%A in ('reg query "%KEY_NAME%" /v "%VALUE_NAME%"') do set "CURRENT_PATH=%%B"
echo Current path: !CURRENT_PATH!

rem Check if adding to path
if /i "%~1"=="add" (
    set "NEW_PATH=!CURRENT_PATH!"

    rem Process each directory to add
    for /L %%i in (0,1,1) do (
        set "DIR_TO_ADD=!PATHS_TO_MANAGE[%%i]!"

        rem Check if path already contains this directory
        echo "!CURRENT_PATH!" | findstr /i /c:"!DIR_TO_ADD!" > nul
        if !ERRORLEVEL!==0 (
            echo !DIR_TO_ADD! already in path
        ) else (
            echo Adding to path: !DIR_TO_ADD!
            set "NEW_PATH=!NEW_PATH!;!DIR_TO_ADD!"
        )
    )

    rem Only update if path was changed
    if "!NEW_PATH!" neq "!CURRENT_PATH!" (
        rem Set the new path in the registry
        reg add "%KEY_NAME%" /v "%VALUE_NAME%" /t REG_EXPAND_SZ /d "!NEW_PATH!" /f
        if !ERRORLEVEL!==0 (
            echo Successfully added Apollo directories to PATH
        ) else (
            echo Failed to add Apollo directories to PATH
        )
    ) else (
        echo No changes needed to PATH
    )
    exit /b !ERRORLEVEL!
)

rem Check if removing from path
if /i "%~1"=="remove" (
    set "REMOVE0=!PATHS_TO_MANAGE[0]!"
    set "REMOVE1=!PATHS_TO_MANAGE[1]!"

    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "try { $key='HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment'; $pathValue=(Get-ItemProperty -Path $key -Name 'Path').Path; $entries = if ($pathValue) { $pathValue -split ';' } else { @() }; $targets = @($env:REMOVE0, $env:REMOVE1) | Where-Object { $_ -and $_.Trim() -ne '' }; $kept = [System.Collections.Generic.List[string]]::new(); $removed = [System.Collections.Generic.List[string]]::new(); foreach ($entry in $entries) { if ([string]::IsNullOrWhiteSpace($entry)) { continue }; if ($targets -contains $entry) { $removed.Add($entry) } else { $kept.Add($entry) } } if ($removed.Count -gt 0) { Set-ItemProperty -Path $key -Name 'Path' -Value ($kept -join ';'); foreach ($entry in $removed) { Write-Output ('Removing from path: ' + $entry) }; Write-Output 'Successfully removed Apollo directories from PATH' } else { Write-Output 'No changes needed to PATH' }; exit 0 } catch { Write-Output 'Failed to remove Apollo directories from PATH'; Write-Output $_.Exception.Message; exit 1 }"
    exit /b !ERRORLEVEL!
)

echo Unknown parameter: %~1
echo Usage: %0 [add^|remove]
exit /b 1
