// SPDX-License-Identifier: LGPL-3.0-or-later
// Reverse-pairing Accept/Reject prompt. Shell_NotifyIcon balloons can't carry
// buttons, so we use a WinRT toast whose buttons are activationType="protocol"
// (a `satellite-pair:` URI) rather than a COM activator -- MinGW ships no WRL.
// The launched process forwards the URI to the running instance.
#pragma once

#include <basetsd.h> // ULONG_PTR (avoid pulling <windows.h> ahead of winsock2.h)

#include <string>

// dwData tag identifying our WM_COPYDATA payload (a satellite-pair: URI).
inline const ULONG_PTR PAIR_URI_COPYDATA = 0x50414952; // 'PAIR'

// Returns false if the WinRT toast API was unavailable; caller falls back to a balloon.
bool showActionablePairToast(const std::string& deviceId, const std::string& deviceName,
                             const std::string& clientIP, const std::string& pin);

// Register the `satellite-pair:` URL scheme (HKCU). Idempotent; call once at startup.
void registerPairProtocol();

// Act on a forwarded `satellite-pair:accept/<id>` or `…/reject/<id>` URI.
void handlePairProtocolUri(const std::string& uri);
