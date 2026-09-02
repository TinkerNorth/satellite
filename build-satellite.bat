@echo off
REM Thin forwarder kept for compatibility: the build logic lives in
REM scripts\build.ps1 (which drives the CMake presets in CMakePresets.json,
REM the same presets CI runs). Usage there: scripts\build.ps1 [debug|release] [test] [-Msvc]
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1" %*
exit /b %ERRORLEVEL%
