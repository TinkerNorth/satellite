// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "globals.h"

namespace lifecycle {

// %LOCALAPPDATA%\TinkerNorth\Satellite\dumps, created on demand.
std::string dumpDir();

// %LOCALAPPDATA%\TinkerNorth\Satellite\logs, created on demand.
std::string logDir();

// %LOCALAPPDATA%\TinkerNorth\Satellite\sentry, created on demand. Local rather
// than roaming: crash-reporter run state is about this machine and has no
// business following the user's profile between them.
std::string sentryDir();

// True if we are the first instance. False if another is already running, in
// which case we ping its HWND_MESSAGE window (titled `appTitle`) and the caller
// should exit without showing UI.
bool acquireSingleInstance(const char* appTitle);

// Idempotent. Caps retained dumps so a leaky build can't fill the disk.
void installCrashHandler();

// Re-asserts satellite's filter over one installed after it, capturing that
// one as the chain target.
//
// Needed because the two crash recorders arm at different times: the local
// minidump goes up in the first lines of WinMain, before any file I/O, while
// Sentry can only arm once the config has been read and the operator's opt-in
// is known. Whoever installs last wins outright, so without this call the
// local dumps\ artifact would be silently replaced. Call once after
// crash::init(). Safe to call when nothing else installed a filter.
void rearmCrashFilterChain();

// Relaunch after an OS-initiated reboot; passes /restart so we can tell a
// recovery launch from a user double-click.
void registerForRestart();

// LoadLibrary resolves only from System32 + app dir, never CWD. Closes the
// classic DLL-planting attack class.
void hardenDllSearchPath();

// Runtime exploit mitigations on top of the link-time flags. Idempotent; logs
// a warning on policies unavailable on older OS.
void applyRuntimeMitigations();

// Idempotent self-heal of the HKCU Run entry from g_config.autoStart, in case
// the installer wrote to the wrong user's hive or the exe moved.
void reconcileAutoStart();

// Spawn the log-file writer (drains the ring to a daily-rotated .log under
// logDir(), 7-day retention). Idempotent; stops when g_appRunning is false.
void startFileLogger();

void stopFileLogger();

} // namespace lifecycle
