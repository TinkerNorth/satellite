@echo off
REM ============================================================================
REM  Install Build Dependencies (Satellite, Windows)
REM
REM  Installs everything needed to build satellite.exe, run the unit tests,
REM  build the Inno Setup installer, and run the documented code-quality tools.
REM
REM    [1] MSYS2:                                   the MinGW-w64 environment
REM    [2] mingw-w64-ucrt-x86_64-gcc:               g++ and windres
REM    [3] mingw-w64-ucrt-x86_64-pkgconf:           pkg-config (library probing)
REM    [4] mingw-w64-ucrt-x86_64-openssl:           OpenSSL (HTTPS client API)
REM    [5] mingw-w64-ucrt-x86_64-opus:              libopus (controller-audio codec)
REM        ([2]-[5] are required for build-satellite.bat and build-tests.bat)
REM    [6] CMake:                                   drives build-satellite.bat
REM    [7] Inno Setup:                              iscc (installer.iss)
REM    [8] LLVM + Cppcheck:                         clang-format, clang-tidy, static analysis
REM    [9] .NET SDK 10:                             dotnet publish
REM        (builds helper\hidmaestro\satellite-hm-helper.exe for the installer)
REM
REM  NOT installed (runtime-only): ViGEmBus driver. Grab it from
REM  https://github.com/nefarius/ViGEmBus/releases to run the receiver and
REM  inject a virtual Xbox 360 controller. The HIDMaestro driver deploys via
REM  the bundled helper (satellite-hm-helper.exe install-driver), elevated.
REM
REM  Requires: winget (ships with Windows 11; in the Microsoft Store as
REM  "App Installer" otherwise). Some installers trigger a UAC prompt; approve
REM  them when asked.
REM ============================================================================

setlocal

set MSYS2_ROOT=C:\msys64
set MINGW_BIN=%MSYS2_ROOT%\ucrt64\bin

echo === Checking winget ===
where winget >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [FAIL] winget not found on PATH.
    echo        Install "App Installer" from the Microsoft Store, then re-run.
    exit /b 1
)
echo [OK]  winget available
echo.

echo === [1/9] MSYS2 ^(provides MinGW-w64 g++ + windres^) ===
winget install --id MSYS2.MSYS2 --silent --accept-source-agreements --accept-package-agreements
echo.

if not exist "%MSYS2_ROOT%\usr\bin\pacman.exe" (
    echo [FAIL] MSYS2 not found at %MSYS2_ROOT% after install.
    echo        If you installed MSYS2 to a different location, run:
    echo            pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-openssl mingw-w64-ucrt-x86_64-opus
    echo        from an MSYS2 shell, then re-run this script.
    exit /b 1
)

echo === [2/9] mingw-w64-ucrt-x86_64-gcc ^(via pacman^) ===
"%MSYS2_ROOT%\usr\bin\pacman.exe" -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc
if %ERRORLEVEL% neq 0 (
    echo [FAIL] pacman could not install mingw-w64-ucrt-x86_64-gcc.
    echo        Try opening "MSYS2 UCRT64" from the Start menu and running:
    echo            pacman -Syu
    echo        then re-run this script.
    exit /b 1
)
echo.

echo === [3/9] mingw-w64-ucrt-x86_64-pkgconf ^(pkg-config, used to locate OpenSSL and libopus^) ===
"%MSYS2_ROOT%\usr\bin\pacman.exe" -S --needed --noconfirm mingw-w64-ucrt-x86_64-pkgconf
if %ERRORLEVEL% neq 0 (
    echo [FAIL] pacman could not install mingw-w64-ucrt-x86_64-pkgconf.
    echo        Try opening "MSYS2 UCRT64" from the Start menu and running:
    echo            pacman -Syu
    echo        then re-run this script.
    exit /b 1
)
echo.

echo === [4/9] mingw-w64-ucrt-x86_64-openssl ^(HTTPS client API^) ===
REM webserver.cpp uses cpp-httplib's SSLServer (CPPHTTPLIB_OPENSSL_SUPPORT), and
REM tls.cpp generates the self-signed cert via libcrypto. Both need OpenSSL
REM headers + static archives shipped by the MSYS2 ucrt64 package.
"%MSYS2_ROOT%\usr\bin\pacman.exe" -S --needed --noconfirm mingw-w64-ucrt-x86_64-openssl
if %ERRORLEVEL% neq 0 (
    echo [FAIL] pacman could not install mingw-w64-ucrt-x86_64-openssl.
    echo        Try opening "MSYS2 UCRT64" from the Start menu and running:
    echo            pacman -Syu
    echo        then re-run this script.
    exit /b 1
)
echo.

echo === [5/9] mingw-w64-ucrt-x86_64-opus ^(controller-audio codec^) ===
"%MSYS2_ROOT%\usr\bin\pacman.exe" -S --needed --noconfirm mingw-w64-ucrt-x86_64-opus
if %ERRORLEVEL% neq 0 (
    echo [FAIL] pacman could not install mingw-w64-ucrt-x86_64-opus.
    echo        Try opening "MSYS2 UCRT64" from the Start menu and running:
    echo            pacman -Syu
    echo        then re-run this script.
    exit /b 1
)
echo.

echo === [6/9] CMake ^(drives build-satellite.bat^) ===
winget install --id Kitware.CMake --silent --accept-source-agreements --accept-package-agreements
echo.

echo === [7/9] Inno Setup ^(installer build^) ===
winget install --id JRSoftware.InnoSetup --silent --accept-source-agreements --accept-package-agreements
echo.

echo === [8/9] LLVM + Cppcheck ^(code-quality tools^) ===
winget install --id LLVM.LLVM --silent --accept-source-agreements --accept-package-agreements
winget install --id Cppcheck.Cppcheck --silent --accept-source-agreements --accept-package-agreements
echo.

echo === [9/9] .NET SDK 10 ^(HIDMaestro helper build^) ===
winget install --id Microsoft.DotNet.SDK.10 --silent --accept-source-agreements --accept-package-agreements
echo.

echo === Adding %MINGW_BIN% to user PATH ===
powershell -NoProfile -Command "$bin = '%MINGW_BIN%'; $p = [Environment]::GetEnvironmentVariable('Path', 'User'); if (-not $p) { $p = '' }; if (($p -split ';') -contains $bin) { Write-Host '[OK]  Already on user PATH' } else { [Environment]::SetEnvironmentVariable('Path', ($p.TrimEnd(';') + ';' + $bin).TrimStart(';'), 'User'); Write-Host '[OK]  Added to user PATH (open a new terminal to pick it up)' }"
if %ERRORLEVEL% neq 0 (
    echo [WARN] Could not update PATH automatically. Add this directory manually:
    echo            %MINGW_BIN%
)
echo.

REM Inno Setup may install per-user (winget default) or per-machine. The
REM per-user install does NOT add itself to PATH automatically, so iscc.exe
REM is invisible to build-installer.bat unless we wire it up here.
echo === Adding Inno Setup to user PATH ===
powershell -NoProfile -Command "$candidates = @((Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6'), (Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Inno Setup 6'), (Join-Path $env:ProgramFiles 'Inno Setup 6')); $iscc = $candidates | Where-Object { Test-Path (Join-Path $_ 'ISCC.exe') } | Select-Object -First 1; if (-not $iscc) { Write-Host '[WARN] ISCC.exe not found in standard Inno Setup install locations.'; exit 0 }; $p = [Environment]::GetEnvironmentVariable('Path', 'User'); if (-not $p) { $p = '' }; if (($p -split ';') -contains $iscc) { Write-Host ('[OK]  Already on user PATH (' + $iscc + ')') } else { [Environment]::SetEnvironmentVariable('Path', ($p.TrimEnd(';') + ';' + $iscc).TrimStart(';'), 'User'); Write-Host ('[OK]  Added ' + $iscc + ' to user PATH (open a new terminal to pick it up)') }"
echo.

echo === Done ===
echo.
echo Next steps:
echo   1. Open a NEW terminal so the PATH change takes effect.
echo   2. Verify:  g++ --version
echo   3. Build:   build-satellite.bat
echo   4. Test:    build-tests.bat
echo.
echo Runtime dependency NOT installed (intentionally):
echo   ViGEmBus: https://github.com/nefarius/ViGEmBus/releases

endlocal
