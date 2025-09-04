@echo off
setlocal

set "RULE_NAME=Sunshine"

rem Check if the rule exists; if not, exit successfully (idempotent)
netsh advfirewall firewall show rule name="%RULE_NAME%" | findstr /C:"No rules match the specified criteria." >nul 2>&1
if %ERRORLEVEL%==0 (
  echo Firewall rule "%RULE_NAME%" not found. Nothing to remove.
  exit /b 0
)

rem Delete the rule; propagate the actual exit code on failure
netsh advfirewall firewall delete rule name="%RULE_NAME%"
exit /b %ERRORLEVEL%
