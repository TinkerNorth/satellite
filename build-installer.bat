@echo off
REM ============================================================================
REM  Build the Windows Inno Setup installer (SatelliteSetup.exe)
REM
REM  Pipeline:
REM    [1] scripts\fetch-redist.ps1  -- downloads + SHA-256 verifies redist\*.exe
REM    [2] scripts\sign.ps1          -- signs satellite.exe (if SATELLITE_SIGN_*
REM                                     or CLOUD_SIGN_TOOL env is set; skipped
REM                                     otherwise with a warning)
REM    [3] iscc installer.iss        -- compiles the installer
REM    [4] scripts\sign.ps1          -- signs SatelliteSetup.exe (same gating)
REM    [5] scripts\generate-sbom.ps1 -- emits dist\satellite-sbom.cdx.json
REM
REM  Steps [2] and [4] no-op cleanly if no signing credentials are
REM  present, so this script is safe to run on a dev machine.
REM
REM  Requires:
REM    - satellite.exe already built (run build-satellite.bat first)
REM    - Inno Setup 6 on PATH       (winget install JRSoftware.InnoSetup)
REM    - PowerShell 5.1+ (ships with Windows)
REM ============================================================================

setlocal

set ROOT=%~dp0
set FETCH=%ROOT%scripts\fetch-redist.ps1
set SIGN=%ROOT%scripts\sign.ps1
set SBOM=%ROOT%scripts\generate-sbom.ps1

if not exist "%ROOT%satellite.exe" (
    echo [FAIL] satellite.exe not found in %ROOT%
    echo        Run build-satellite.bat first.
    exit /b 1
)

echo === [1/5] Fetching redistributables ===
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%FETCH%"
if %ERRORLEVEL% neq 0 (
    echo [FAIL] fetch-redist.ps1
    exit /b 1
)
echo.

echo === [2/5] Signing satellite.exe ===
REM Skip if no signing creds are configured -- dev builds should still work.
if not defined SATELLITE_SIGN_THUMBPRINT if not defined SATELLITE_SIGN_PFX if not defined CLOUD_SIGN_TOOL (
    echo [SKIP] No signing credentials in env -- satellite.exe will be unsigned.
    echo        SmartScreen will warn first-time users. To enable signing:
    echo          set SATELLITE_SIGN_THUMBPRINT=...  ^(local hardware/store cert^)
    echo          set SATELLITE_SIGN_PFX=...^&^& set SATELLITE_SIGN_PFX_PASSWORD=...
    echo          set CLOUD_SIGN_TOOL="AzureSignTool sign ..."  ^(cloud signing^)
    goto :compile
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SIGN%" -Files "satellite.exe"
if %ERRORLEVEL% neq 0 (
    echo [FAIL] sign satellite.exe
    exit /b 1
)
echo.

:compile
echo === [3/5] Compiling installer (iscc installer.iss) ===

REM Locate iscc.exe. Prefer PATH; fall back to standard install locations
REM so a fresh shell after a per-user winget install of Inno Setup just
REM works without the user having to fight with PATH updates.
set "ISCC_EXE="
for /f "delims=" %%I in ('where iscc.exe 2^>nul') do set "ISCC_EXE=%%I"
if not defined ISCC_EXE if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC_EXE=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC_EXE if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC_EXE=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC_EXE if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC_EXE=%ProgramFiles%\Inno Setup 6\ISCC.exe"

if not defined ISCC_EXE (
    echo [FAIL] iscc.exe not found on PATH or in standard install locations.
    echo        Install Inno Setup 6: winget install JRSoftware.InnoSetup
    echo        ^(or run install-dependencies.bat^)
    exit /b 1
)

echo [INFO] Using ISCC: %ISCC_EXE%
"%ISCC_EXE%" "%ROOT%installer.iss"
if %ERRORLEVEL% neq 0 (
    echo [FAIL] iscc
    exit /b 1
)
echo.

echo === [4/5] Signing SatelliteSetup.exe ===
if not defined SATELLITE_SIGN_THUMBPRINT if not defined SATELLITE_SIGN_PFX if not defined CLOUD_SIGN_TOOL (
    echo [SKIP] No signing credentials in env -- SatelliteSetup.exe will be unsigned.
    goto :sbom
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SIGN%" -Files "dist\SatelliteSetup.exe"
if %ERRORLEVEL% neq 0 (
    echo [FAIL] sign SatelliteSetup.exe
    exit /b 1
)
echo.

:sbom
echo === [5/5] Generating SBOM ===
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SBOM%"
if %ERRORLEVEL% neq 0 (
    echo [WARN] sbom generation failed -- continuing anyway
)
echo.

echo === Installer built: dist\SatelliteSetup.exe ===

endlocal
