@echo off
REM ============================================================================
REM  Build & Run Tests (Satellite) -- thin CMake/CTest wrapper.
REM
REM  This script used to hand-compile a single test binary, so the other suites
REM  registered in CMakeLists.txt (receiver, mdns, pairing, vigem_adapter, ...)
REM  silently never ran here. CMakeLists.txt is the single source of truth for
REM  what gets built and tested; this wrapper just drives it.
REM
REM  Requires: CMake + a MinGW-w64 g++ (MSYS2) on PATH (see install-dependencies.bat).
REM ============================================================================

setlocal

where cmake >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [FAIL] cmake not found on PATH. Install CMake and MinGW-w64 g++ first;
    echo        see install-dependencies.bat.
    exit /b 1
)

echo === [1/3] Configure (Release, MinGW Makefiles) ===
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% neq 0 (
    echo [FAIL] configure
    exit /b 1
)

echo === [2/3] Build ===
cmake --build build --config Release -j
if %ERRORLEVEL% neq 0 (
    echo [FAIL] build
    exit /b 1
)

echo === [3/3] Run tests (ctest) ===
ctest --test-dir build --output-on-failure -C Release
if %ERRORLEVEL% neq 0 (
    echo.
    echo === Tests FAILED ===
    exit /b 1
)

echo.
echo === All tests passed ===
