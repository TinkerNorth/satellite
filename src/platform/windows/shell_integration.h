// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * shell_integration.h -- Modern Win10/11 shell-integration plumbing.
 *
 * Everything in this header revolves around one thing: the
 * AppUserModelID (AUMID). Windows uses the AUMID as the stable
 * identity of an application across taskbar pinning, jump lists,
 * toast notifications routed through Action Center, and the Start
 * menu's "recently launched" surface. Without it, none of those
 * features behave correctly -- taskbar items don't group, toasts
 * fall back to legacy balloons that vanish, and the Start menu
 * shows orphan entries.
 *
 * We pick a stable string ("TinkerNorth.Satellite") and call
 * SetCurrentProcessExplicitAppUserModelID() exactly once at startup,
 * before any window or tray icon is created. The same string is
 * baked into the installer-created Start Menu shortcut (via Inno
 * Setup), which is what teaches Windows the AUMID -> .exe mapping
 * needed for toasts to survive process restarts.
 */
#pragma once

#include "globals.h"

#include <guiddef.h>

namespace shell_integration {

// Stable AUMID. Must match the System.AppUserModel.ID property on the
// installer-created shortcut (set via Inno Setup's [Icons] AppUserModelID
// or a post-install IPropertyStore write). Changing this orphans every
// pinned-taskbar entry and pending toast in Action Center for existing
// users, so treat it like a primary key.
inline constexpr const wchar_t* kAppUserModelID = L"TinkerNorth.Satellite";

// Stable GUID for the tray icon's NIF_GUID identity. Lets Explorer
// remember the user's tray-hide / promotion choices across reinstalls
// and updates -- with just uID, every fresh install path looks like a
// new app and re-prompts. Generated once; never reissue.
//
// {7A2C9F1E-3D5B-4E6F-8A9B-1C2D3E4F5061}
inline constexpr GUID kTrayIconGuid = {
    0x7a2c9f1e, 0x3d5b, 0x4e6f, {0x8a, 0x9b, 0x1c, 0x2d, 0x3e, 0x4f, 0x50, 0x61}};

// Call once at WinMain entry, before any HWND / tray icon. Idempotent.
//
// This is the load-bearing call for *every* modern shell feature --
// without it Explorer falls back to per-window heuristics that group
// our tray balloons, taskbar entries and jump-list invocations under
// the wrong identity (sometimes "satellite.exe", sometimes the parent
// shell). Returns true on success; logs a warning on failure but
// doesn't block startup.
bool registerAppUserModelID();

// Rebuild the taskbar jump list (right-click on pinned/running taskbar
// button). Idempotent and cheap -- safe to call on every launch and
// after any state change that would affect the entries.
//
// Currently registers three user tasks:
//   * Open Web UI       -> http://localhost:<webPort>
//   * Open Logs Folder  -> explorer.exe %LOCALAPPDATA%\TinkerNorth\Satellite\logs
//   * Check for Updates -> satellite.exe --check-updates
//
// Returns true on success. Failure is non-fatal (Win7 lacks
// ICustomDestinationList; we just log + skip).
bool refreshJumpList();

// Show a Win10/11 toast notification. Routes through Shell_NotifyIcon's
// NIF_INFO path, which Explorer auto-promotes to a toast in Action
// Center *when* the calling process has a registered AUMID (see
// registerAppUserModelID above) -- otherwise it degrades to a legacy
// balloon. Either way: one call, no WinRT dependency.
//
// `title` and `body` are UTF-8; we widen internally. Both truncate
// silently to the platform limits (64 / 256 chars) -- callers should
// keep them short for tooltip-style nudges anyway.
void showToast(const std::string& title, const std::string& body);

} // namespace shell_integration
