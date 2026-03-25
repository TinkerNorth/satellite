@echo off
REM ============================================================================
REM  Build Satellite (Windows)
REM  Requires: MinGW-w64 (g++) in PATH
REM ============================================================================

setlocal

set CXX=g++
set CXXFLAGS=-O2 -Wall -Wextra -std=c++17 -D_WIN32_WINNT=0x0A00 -static
set INCLUDES=-Ivigem/include -Ilib
set SRC_FILES=src/main.cpp src/globals.cpp src/config.cpp src/crypto.cpp src/vigem.cpp src/receiver.cpp src/webserver.cpp src/pairing.cpp src/discovery.cpp src/tray.cpp

echo === Building Satellite ===
echo.

echo [1/1] satellite.exe
%CXX% %CXXFLAGS% %INCLUDES% -Isrc -DCPPHTTPLIB_NO_EXCEPTIONS -o satellite.exe %SRC_FILES% -lsetupapi -lws2_32 -lshell32 -lole32 -ladvapi32 -lbcrypt -lcrypt32 -mwindows
if %ERRORLEVEL% neq 0 (
    echo [FAIL] satellite.exe
    exit /b 1
)
echo [OK]  satellite.exe

echo.
echo === Build complete ===

