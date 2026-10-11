@echo off

rem Get sunshine root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set RULE_NAME=Apollo
set PROGRAM_BIN="%ROOT_DIR%\sunshine.exe"

rem Add the rule
netsh advfirewall firewall add rule name=%RULE_NAME% dir=in action=allow protocol=tcp program=%PROGRAM_BIN% enable=yes
netsh advfirewall firewall add rule name=%RULE_NAME% dir=in action=allow protocol=udp program=%PROGRAM_BIN% enable=yes

rem Eclipse Link (aurora-link.exe) is a separate executable, so the rule above does not cover it. Without its own
rem exclusion Windows silently blocks its inbound UDP pad port. UDP only: its one inbound listener is that port.
set LINK_RULE_NAME="Eclipse Link"
set LINK_PROGRAM_BIN="%ROOT_DIR%\aurora-link.exe"
netsh advfirewall firewall add rule name=%LINK_RULE_NAME% dir=in action=allow protocol=udp program=%LINK_PROGRAM_BIN% enable=yes
