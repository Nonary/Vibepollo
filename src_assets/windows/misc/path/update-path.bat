@echo off
setlocal DisableDelayedExpansion

rem Check if parameter is provided
if "%~1"=="" (
    echo Usage: %~nx0 [add^|remove]
    echo   add    - Adds Apollo directories to system PATH
    echo   remove - Removes Apollo directories from system PATH
    exit /b 1
)

if /i not "%~1"=="add" if /i not "%~1"=="remove" (
    echo Unknown parameter: %~1
    echo Usage: %~nx0 [add^|remove]
    exit /b 1
)

set "POWERSHELL_EXE=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%POWERSHELL_EXE%" set "POWERSHELL_EXE=powershell.exe"

set "UPDATE_PATH_SCRIPT=%~dp0update-path.ps1"
if not exist "%UPDATE_PATH_SCRIPT%" (
    echo Missing PATH helper script: %UPDATE_PATH_SCRIPT%
    exit /b 1
)

"%POWERSHELL_EXE%" -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -File "%UPDATE_PATH_SCRIPT%" -Action "%~1"
exit /b %ERRORLEVEL%
