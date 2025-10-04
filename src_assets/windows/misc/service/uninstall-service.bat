@echo off
setlocal enabledelayedexpansion

set "SERVICE_CONFIG_DIR=%LOCALAPPDATA%\SudoMaker\Apollo"
set "SERVICE_CONFIG_FILE=%SERVICE_CONFIG_DIR%\service_start_type.txt"

rem Save the current service start type to a file if the service exists
sc qc ApolloService >nul 2>&1
if %ERRORLEVEL%==0 (
    if not exist "%SERVICE_CONFIG_DIR%\" mkdir "%SERVICE_CONFIG_DIR%\"

    rem Get the start type
    for /f "tokens=3" %%i in ('sc qc ApolloService ^| findstr /C:"START_TYPE"') do (
        set "CURRENT_START_TYPE=%%i"
    )

    rem Set the content to write
    if "!CURRENT_START_TYPE!"=="2" (
        sc qc ApolloService | findstr /C:"(DELAYED)" >nul
        if !ERRORLEVEL!==0 (
            set "CONTENT=2-delayed"
        ) else (
            set "CONTENT=2"
        )
    ) else if "!CURRENT_START_TYPE!" NEQ "" (
        set "CONTENT=!CURRENT_START_TYPE!"
    ) else (
        set "CONTENT=unknown"
    )

    rem Write content to file
    echo !CONTENT!> "%SERVICE_CONFIG_FILE%"
)

rem Stop and delete the legacy SunshineSvc service (best-effort)
net stop sunshinesvc >nul 2>&1
for /L %%i in (1,1,15) do (
  sc query sunshinesvc | findstr /C:"STATE" | findstr /C:"STOPPED" >nul 2>&1 && goto :legacy_stopped
  timeout /t 1 >nul
)
:legacy_stopped
sc delete sunshinesvc >nul 2>&1

rem Stop and delete the new ApolloService service
net stop ApolloService
sc delete ApolloService
