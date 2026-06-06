// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * toast.h — modern Windows (WinRT) actionable toast for pairing requests.
 *
 * The legacy Shell_NotifyIcon balloon can't carry buttons, so a reverse-pairing
 * prompt with inline Accept / Reject uses a real toast (Windows.UI.Notifications)
 * built from XML. The buttons are `activationType="protocol"` so a click
 * launches a `satellite-pair:` URI rather than needing a COM activator (MinGW
 * ships no WRL); registerPairProtocol() registers the scheme, and the launched
 * process forwards the URI to the running instance, which calls
 * handlePairProtocolUri().
 */
#pragma once

#include <basetsd.h> // ULONG_PTR (avoid pulling <windows.h> ahead of winsock2.h)

#include <string>

// dwData tag identifying our WM_COPYDATA payload (a satellite-pair: URI).
inline const ULONG_PTR PAIR_URI_COPYDATA = 0x50414952; // 'PAIR'

// Show an actionable toast for a pending request. Returns true if the toast was
// handed to the platform; false if the WinRT toast API was unavailable (the
// caller should then fall back to a balloon).
bool showActionablePairToast(const std::string& deviceId, const std::string& deviceName,
                             const std::string& clientIP, const std::string& pin);

// Register the `satellite-pair:` URL scheme (HKCU) so the toast buttons route
// back to us. Idempotent; call once at startup.
void registerPairProtocol();

// Act on a forwarded `satellite-pair:accept/<id>` or `…/reject/<id>` URI.
void handlePairProtocolUri(const std::string& uri);
