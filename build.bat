@echo off
REM ============================================================================
REM  Build script for controller-forward
REM  Requires: MinGW-w64 (g++) in PATH
REM ============================================================================

setlocal

set CXX=g++
set CXXFLAGS=-O2 -Wall -Wextra -std=c++17
set INCLUDES=-Ivigem/include

echo === Building controller-forward ===
echo.

echo [1/2] controller-receiver.exe
%CXX% %CXXFLAGS% %INCLUDES% -o controller-receiver.exe controller-receiver.cpp -lsetupapi -lws2_32
if %ERRORLEVEL% neq 0 (
    echo [FAIL] controller-receiver.exe
    exit /b 1
)
echo [OK]  controller-receiver.exe

echo [2/2] controller-sender.exe
%CXX% %CXXFLAGS% -o controller-sender.exe controller-sender.cpp -lxinput1_4 -lws2_32
if %ERRORLEVEL% neq 0 (
    echo [FAIL] controller-sender.exe
    exit /b 1
)
echo [OK]  controller-sender.exe

echo.
echo === Build complete ===

