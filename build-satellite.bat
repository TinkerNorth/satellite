@echo off
REM ============================================================================
REM  Build Satellite (Windows)
REM  Requires: MinGW-w64 (g++) in PATH
REM ============================================================================

setlocal

set CXX=g++
REM Compiler-side hardening:
REM   -fstack-protector-strong  stack-canary smash-detection on functions
REM                             with risky frames (arrays, refs, addrs).
REM   -D_FORTIFY_SOURCE=2       overflow-checked str*/mem*/printf at -O2+.
REM   -Wformat -Wformat-security catch printf-style format-string holes.
REM Link-side hardening (passed via -Wl,):
REM   --dynamicbase             ASLR.
REM   --high-entropy-va         64-bit-wide ASLR slide (vs 32-bit fallback).
REM   --nxcompat                DEP / NX bit.
REM   --disable-auto-image-base let the loader pick the base each run.
REM Disable insecure stdio relinks (no _s suffixes baked into MinGW headers).
set CXXFLAGS=-O2 -Wall -Wextra -std=c++17 -D_WIN32_WINNT=0x0A00 -D_FORTIFY_SOURCE=2 -fstack-protector-strong -Wformat -Wformat-security -static
set LDFLAGS=-Wl,--dynamicbase -Wl,--high-entropy-va -Wl,--nxcompat -Wl,--disable-auto-image-base
REM Vendored third-party headers (lib/httplib.h, vigem/include, libsodium) go in
REM via -isystem so g++ suppresses their warnings -- we don't control that code
REM and the upstream cpp-httplib uses its own deprecated APIs internally.
set INCLUDES=-Isrc/platform/windows -isystem vigem/include -isystem lib -isystem lib/libsodium/libsodium-win64/include
set LIBDIRS=-Llib/libsodium/libsodium-win64/lib
REM Source files: platform layer + portable core (session_service / update_service / github_release)
REM + net layer + adapters + the per-OS updater adapter. `windres` consumes satellite.rc
REM separately below (which now #includes src/core/version.h — resolved relative to the .rc
REM file's own directory, so no -I flag needed for the resource step).
set SRC_FILES=src/platform/windows/main.cpp src/platform/windows/globals.cpp src/platform/windows/config.cpp src/platform/windows/crypto.cpp src/platform/windows/vigem.cpp src/platform/windows/tray.cpp src/platform/windows/toast.cpp src/platform/windows/app_lifecycle.cpp src/platform/windows/shell_integration.cpp src/platform/windows/vigem_adapter.cpp src/platform/windows/gamepad_backend.cpp src/platform/windows/updater_adapter.cpp src/adapters/client_adapter.cpp src/net/receiver.cpp src/net/inner_dispatch.cpp src/net/webserver.cpp src/net/tls.cpp src/net/machine_id.cpp src/net/pairing.cpp src/net/pairing_service.cpp src/net/discovery.cpp src/net/mdns_protocol.cpp src/net/mdns_responder.cpp src/core/session_service.cpp src/core/update_service.cpp src/core/github_release.cpp src/adapters/log_adapter.cpp

echo === Building Satellite ===
echo.

echo [1/2] Compiling resources
windres satellite.rc -o satellite_res.o
if %ERRORLEVEL% neq 0 (
    echo [FAIL] resources
    exit /b 1
)
echo [OK]  resources

echo [2/2] satellite.exe
REM The client API server speaks HTTPS via cpp-httplib's SSLServer, gated on
REM CPPHTTPLIB_OPENSSL_SUPPORT. OpenSSL (ssl + crypto) is provided by the
REM MSYS2 ucrt64 package -- see install-dependencies.bat. Static OpenSSL also
REM needs -lgdi32 and -luser32 (already implicit, listed for clarity).
REM Additional libraries: -lDbgHelp for MiniDumpWriteDump (crash handler in
REM src/platform/windows/app_lifecycle.cpp). All other dependencies of the
REM lifecycle helpers (RegisterApplicationRestart, SetDefaultDllDirectories,
REM CreateMutex, etc.) come from kernel32 which is implicit.
REM Additional libraries beyond the platform default:
REM   -lruntimeobject WinRT RoActivateInstance / RoGetActivationFactory + HSTRING
REM                 (WindowsCreateString/WindowsDeleteString) for the actionable
REM                 reverse-pairing toast (src/platform/windows/toast.cpp).
REM   -lwintrust    WinVerifyTrust on OTA-downloaded installer (updater_adapter)
REM   -lpropsys     IPropertyStore + PKEY_AppUserModel_ID (shell_integration)
REM   -lcomctl32    TaskDialogIndirect (main.cpp's fatal-error dialog)
REM   -lcomdlg32    Common dialogs (TaskDialogIndirect dependency on some SDKs)
REM -luuid    in-library copies of GUIDs / PROPERTYKEYs we reference by
REM           symbol (FOLDERID_*, CLSID_ShellLink, IID_IShellLinkW,
REM           CLSID_DestinationList, PKEY_Title, PKEY_AppUserModel_ID).
REM           Without it: undefined references to FOLDERID_* / IID_*.
REM -lshlwapi InitPropVariantFromString is an inline shim around
REM           SHStrDupW (in shlwapi). Without it ld silently fails the
REM           link without printing a diagnostic.
%CXX% %CXXFLAGS% %LDFLAGS% %INCLUDES% %LIBDIRS% -Isrc -DCPPHTTPLIB_NO_EXCEPTIONS -DCPPHTTPLIB_OPENSSL_SUPPORT -o satellite.exe %SRC_FILES% satellite_res.o -lsodium -lssl -lcrypto -lsetupapi -lws2_32 -lshell32 -lole32 -lruntimeobject -ladvapi32 -lbcrypt -lcrypt32 -lwinmm -lwinhttp -lwintrust -lpropsys -lshlwapi -lcomctl32 -lcomdlg32 -luuid -lgdi32 -luser32 -lDbgHelp -mwindows
if %ERRORLEVEL% neq 0 (
    echo [FAIL] satellite.exe
    exit /b 1
)
echo [OK]  satellite.exe

echo.
echo === Build complete ===

