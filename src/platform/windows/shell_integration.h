// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "globals.h"

#include <guiddef.h>

namespace shell_integration {

// Stable identity for taskbar grouping, jump lists, and Action Center toasts.
// Must match the System.AppUserModel.ID on the installer-created shortcut;
// changing it orphans every pinned entry and pending toast -- treat as a
// primary key.
inline constexpr const wchar_t* kAppUserModelID = L"TinkerNorth.Satellite";

// Tray-icon NIF_GUID identity. Lets Explorer remember tray-hide/promotion
// choices across reinstalls (uID alone re-prompts every install). Never reissue.
inline constexpr GUID kTrayIconGuid = {
    0x7a2c9f1e, 0x3d5b, 0x4e6f, {0x8a, 0x9b, 0x1c, 0x2d, 0x3e, 0x4f, 0x50, 0x61}};

// Call once at WinMain entry, before any HWND / tray icon. Idempotent. Without
// it Explorer groups our shell surfaces under the wrong identity. Non-blocking.
bool registerAppUserModelID();

// Rebuild the taskbar jump list. Idempotent and cheap; safe every launch.
// Non-fatal on failure (Win7 lacks ICustomDestinationList).
bool refreshJumpList();

// Win10/11 toast via Shell_NotifyIcon NIF_INFO, which Explorer promotes to an
// Action Center toast only when an AUMID is registered (else a legacy balloon).
// title/body are UTF-8; both truncate silently to 64 / 256 chars.
void showToast(const std::string& title, const std::string& body);

} // namespace shell_integration
