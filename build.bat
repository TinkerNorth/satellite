@echo off
REM ============================================================================
REM  Build script for controller-forward
REM  Requires: MinGW-w64 (g++) in PATH
REM ============================================================================

setlocal

set CXX=g++
set CXXFLAGS=-O2 -Wall -Wextra -std=c++17 -D_WIN32_WINNT=0x0A00 -static
set INCLUDES=-Ivigem/include -Ilib

echo === Building controller-forward ===
echo.

set SRC_FILES=src/main.cpp src/globals.cpp src/config.cpp src/crypto.cpp src/vigem.cpp src/receiver.cpp src/webserver.cpp src/pairing.cpp src/discovery.cpp src/tray.cpp

echo [1/2] controller-receiver.exe  (tray + web UI)
%CXX% %CXXFLAGS% %INCLUDES% -Isrc -DCPPHTTPLIB_NO_EXCEPTIONS -o controller-receiver.exe %SRC_FILES% -lsetupapi -lws2_32 -lshell32 -lole32 -ladvapi32 -lbcrypt -lcrypt32 -mwindows
if %ERRORLEVEL% neq 0 (
    echo [FAIL] controller-receiver.exe
    exit /b 1
)
echo [OK]  controller-receiver.exe

echo [2/2] controller-sender.exe
%CXX% %CXXFLAGS% -o controller-sender.exe controller-sender.cpp -lxinput1_4 -lws2_32 -lshell32 -lole32
if %ERRORLEVEL% neq 0 (
    echo [FAIL] controller-sender.exe
    exit /b 1
)
echo [OK]  controller-sender.exe

echo.
echo === Build complete ===

