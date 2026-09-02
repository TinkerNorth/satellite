@echo off
REM Thin forwarder kept for compatibility: builds Release and runs the full
REM ctest suite via scripts\build.ps1 (which drives the CMake presets in
REM CMakePresets.json, the same presets CI runs).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1" release test
exit /b %ERRORLEVEL%
