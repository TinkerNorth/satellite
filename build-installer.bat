@echo off
REM ============================================================================
REM  Build the Windows Inno Setup installer (SatelliteSetup.exe)
REM
REM  This is the canonical Windows entry point for producing dist\SatelliteSetup.exe.
REM  It runs the two steps the installer pipeline needs, in order:
REM    [1] scripts\fetch-redist.ps1  -- downloads + SHA-256 verifies redist\*.exe
REM    [2] iscc installer.iss        -- compiles the installer
REM
REM  Requires:
REM    - satellite.exe already built (run build-satellite.bat first)
REM    - Inno Setup 6 on PATH       (winget install JRSoftware.InnoSetup)
REM    - PowerShell 5.1+ (ships with Windows)
REM ============================================================================

setlocal

set ROOT=%~dp0
set FETCH=%ROOT%scripts\fetch-redist.ps1

if not exist "%ROOT%satellite.exe" (
    echo [FAIL] satellite.exe not found in %ROOT%
    echo        Run build-satellite.bat first.
    exit /b 1
)

echo === [1/2] Fetching redistributables ===
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%FETCH%"
if %ERRORLEVEL% neq 0 (
    echo [FAIL] fetch-redist.ps1
    exit /b 1
)
echo.

echo === [2/2] Compiling installer ^(iscc installer.iss^) ===
where iscc >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [FAIL] iscc not found on PATH.
    echo        Install Inno Setup 6: winget install JRSoftware.InnoSetup
    exit /b 1
)
iscc "%ROOT%installer.iss"
if %ERRORLEVEL% neq 0 (
    echo [FAIL] iscc
    exit /b 1
)

echo.
echo === Installer built: dist\SatelliteSetup.exe ===

endlocal
