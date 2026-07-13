// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "globals.h"

#include <guiddef.h>

namespace shell_integration {

// Must match the System.AppUserModel.ID on the installer-created shortcut;
// changing it orphans every pinned entry and pending toast. Treat as a primary key.
inline constexpr const wchar_t* kAppUserModelID = L"TinkerNorth.Satellite";

// NIF_GUID lets Explorer remember tray-hide/promotion choices across reinstalls
// (uID alone re-prompts every install). Never reissue.
inline constexpr GUID kTrayIconGuid = {
    0x7a2c9f1e, 0x3d5b, 0x4e6f, {0x8a, 0x9b, 0x1c, 0x2d, 0x3e, 0x4f, 0x50, 0x61}};

// Call once at WinMain entry, before any HWND/tray icon. Idempotent.
bool registerAppUserModelID();

// Non-fatal on failure (Win7 lacks ICustomDestinationList).
bool refreshJumpList();

// Explorer promotes NIF_INFO to an Action Center toast only when an AUMID is
// registered (else a legacy balloon). title/body UTF-8; truncate to 64/256 chars.
void showToast(const std::string& title, const std::string& body);

} // namespace shell_integration
