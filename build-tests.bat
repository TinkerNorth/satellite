@echo off
REM ============================================================================
REM  Build & Run Tests (Satellite)
REM  Requires: MinGW-w64 (g++) in PATH
REM ============================================================================

setlocal

set CXX=g++
set CXXFLAGS=-O2 -Wall -Wextra -std=c++17 -D_WIN32_WINNT=0x0A00
set INCLUDES=-Isrc

echo === Building Tests ===
echo.

echo [1/1] test_session_service.exe
%CXX% %CXXFLAGS% %INCLUDES% -o test_session_service.exe tests/test_session_service.cpp src/core/session_service.cpp
if %ERRORLEVEL% neq 0 (
    echo [FAIL] test_session_service.exe
    exit /b 1
)
echo [OK]  test_session_service.exe

echo.
echo === Running Tests ===
echo.

test_session_service.exe
if %ERRORLEVEL% neq 0 (
    echo.
    echo === Tests FAILED ===
    exit /b 1
)

echo.
echo === All tests passed ===

