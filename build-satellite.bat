@echo off
REM ============================================================================
REM  Build Satellite (Windows) -- thin CMake wrapper.
REM
REM  This script used to carry its own hand-maintained source + library lists,
REM  which drifted from CMakeLists.txt (a missing entry surfaced as a late link
REM  error). CMakeLists.txt is now the single source of truth; this wrapper just
REM  drives it, so the documented `build-satellite.bat` entry point still works
REM  and there is nothing left to keep in sync.
REM
REM  Requires: CMake + a MinGW-w64 g++ (MSYS2) on PATH (see install-dependencies.bat).
REM  Output:   satellite.exe at the repo root (CMake RUNTIME_OUTPUT_DIRECTORY).
REM
REM  For the hardened MSVC build instead (CFG + CET + Spectre), use:
REM      cmake --preset windows-msvc
REM      cmake --build build-msvc --config Release
REM  (needs Visual Studio + vcpkg; see CMakePresets.json / vcpkg.json).
REM ============================================================================

setlocal

where cmake >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [FAIL] cmake not found on PATH. Install CMake and MinGW-w64 g++ first;
    echo        see install-dependencies.bat.
    exit /b 1
)

echo === [1/2] Configure (Release, MinGW Makefiles) ===
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% neq 0 (
    echo [FAIL] configure
    exit /b 1
)

echo === [2/2] Build satellite.exe ===
cmake --build build --config Release -j
if %ERRORLEVEL% neq 0 (
    echo [FAIL] build
    exit /b 1
)

echo.
echo === Build complete: satellite.exe ===
