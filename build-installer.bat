@echo off
REM Thin forwarder kept for compatibility: the installer pipeline lives in
REM scripts\build-installer.ps1 (fetch-redist, helper publish, optional
REM signing, iscc with /DMyAppVersion=<VERSION>, SBOM).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build-installer.ps1" %*
exit /b %ERRORLEVEL%
