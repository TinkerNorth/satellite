// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * app_lifecycle.h -- Windows process-lifecycle primitives.
 *
 * Bundles the boring-but-important Win32 startup hygiene that every
 * top-tier desktop app needs but main.cpp shouldn't have to inline:
 *   * single-instance guard via a kernel mutex
 *   * unhandled-exception filter that writes a minidump
 *   * RegisterApplicationRestart so the shell auto-relaunches us after
 *     an OS-initiated reboot (Windows Update, Restart Manager, etc.)
 *   * heap and DLL-search hardening (link-time + runtime layers)
 *   * persistent rolling log file (mirrors the in-memory ring)
 *
 * Each function returns a clear go/no-go signal and is safe to call
 * before any other process-wide state exists.
 */
#pragma once

#include "globals.h"

namespace lifecycle {

// Path to %LOCALAPPDATA%\TinkerNorth\Satellite\dumps -- created on demand.
// Exposed so the in-app "Report a problem" UI can link there.
std::string dumpDir();

// Path to %LOCALAPPDATA%\TinkerNorth\Satellite\logs -- created on demand.
std::string logDir();

// Acquire the single-instance mutex. Returns true if we are the first
// instance and should keep running. Returns false if another instance is
// already running -- in that case we ping its hidden window (so it can
// flash a tray balloon or open the web UI) and the caller should exit
// without showing UI itself.
//
// `appTitle` is the WindowText of the existing instance's HWND_MESSAGE
// window so we can ShellExecute/PostMessage to it.
bool acquireSingleInstance(const char* appTitle);

// Install SetUnhandledExceptionFilter that writes a .dmp file into
// dumpDir() and lets the OS continue to terminate the process. Idempotent.
// Also enforces a small retention cap so a leaky build can't fill the
// user's disk with crash dumps.
void installCrashHandler();

// Tell the shell to relaunch us after an OS-initiated reboot. Picks up
// the optional /restart command-line switch on relaunch so we know we
// were brought back automatically (we can log it / skip splash, etc.).
void registerForRestart();

// Process-wide DLL search-path lockdown -- LoadLibrary only resolves
// from System32 and the application directory, never CWD. Closes the
// classic Win32 DLL-planting attack class.
void hardenDllSearchPath();

// Belt-and-braces runtime exploit mitigations on top of the link-time
// flags (--dynamicbase, --nxcompat, --high-entropy-va, /CETCOMPAT).
// Enforces ProcessSignaturePolicy (signed-only DLLs from system
// directories), ProcessImageLoadPolicy (no remote / low-mandatory-IL
// image loads), and ProcessDynamicCodePolicy where the binary doesn't
// JIT. Idempotent; logs a warning on unavailable policies (older OS).
void applyRuntimeMitigations();

// Idempotent: if g_config.autoStart is true, ensure HKCU\...\Run\Satellite
// points at this exe (quoted). If g_config.autoStart is false, delete it.
// Called once at startup so the registry self-heals after a move/upgrade
// where the installer might have written to the wrong user's hive.
void reconcileAutoStart();

// Start the background log-file writer. Spawns one thread that drains
// the in-memory ring buffer into a daily-rotated .log file under
// logDir(), keeping the last 7 days. Safe to call once; subsequent
// calls no-op. Stops automatically when g_appRunning flips to false.
void startFileLogger();

} // namespace lifecycle
