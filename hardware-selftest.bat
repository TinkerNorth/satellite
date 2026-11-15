@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\hardware-selftest.ps1" %*
exit /b %ERRORLEVEL%
