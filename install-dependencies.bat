@echo off
REM Thin forwarder kept for compatibility: the dependency installer lives in
REM scripts\install-deps.ps1. It installs the MSYS2 MINGW64 toolchain CI
REM builds with (an older version of this script installed UCRT64, a
REM different toolchain than CI's); pass -Msvc to also bootstrap the
REM hardened MSVC + vcpkg lane.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install-deps.ps1" %*
exit /b %ERRORLEVEL%
