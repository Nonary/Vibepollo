@echo off
setlocal

rem Get sunshine root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set "RULE_NAME=Apollo"
set "PROGRAM_BIN=%ROOT_DIR%\sunshine.exe"

rem Ensure idempotency: remove any existing rule with the same name (ignore errors)
netsh advfirewall firewall delete rule name="%RULE_NAME%" >nul 2>&1

rem Add the rule(s)
netsh advfirewall firewall add rule name="%RULE_NAME%" dir=in action=allow protocol=tcp program="%PROGRAM_BIN%" enable=yes
netsh advfirewall firewall add rule name="%RULE_NAME%" dir=in action=allow protocol=udp program="%PROGRAM_BIN%" enable=yes

exit /b %ERRORLEVEL%
