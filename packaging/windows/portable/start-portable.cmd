@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Clear an inherited variable that would override CMD's dynamic exit status.
set "ERRORLEVEL="
cd /d "%~dp0"
set "PATH=%~dp0;%~dp0tools;%PATH%"
if not exist "sunshine.exe" goto incomplete
if not exist "config\sunshine.conf" goto incomplete
set "portable_write_test=config\.portable-write-%RANDOM%-%RANDOM%.tmp"
if exist "%portable_write_test%" goto unwritable
> "%portable_write_test%" echo writable
if errorlevel 1 goto unwritable
if not exist "%portable_write_test%" goto unwritable
del "%portable_write_test%" >nul 2>&1
"sunshine.exe" "config\sunshine.conf"
set "portable_exit_code=%errorlevel%"
if not "%portable_exit_code%"=="0" pause
exit /b %portable_exit_code%

:incomplete
echo The portable package is incomplete. Extract the complete ZIP before starting.
pause
exit /b 1

:unwritable
echo Extract the complete portable package to a folder you can write to.
pause
exit /b 1
