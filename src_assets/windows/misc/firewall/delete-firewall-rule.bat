@echo off

set RULE_NAME=Apollo

rem Delete the rule
netsh advfirewall firewall delete rule name=%RULE_NAME%
rem Eclipse Link's exclusion is a separate rule name, so it needs its own delete.
netsh advfirewall firewall delete rule name="Eclipse Link"
