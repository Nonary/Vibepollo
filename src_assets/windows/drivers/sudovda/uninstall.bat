@echo off
setlocal
pushd %~dp0

if not exist "nefconc.exe" (
    echo [SudoVDA] Skipping removal: nefconc.exe is missing.
    goto :end
)

nefconc.exe --remove-device-node --hardware-id root\sudomaker\sudovda --class-guid "4D36E968-E325-11CE-BFC1-08002BE10318"

:end
popd
exit /b 0
