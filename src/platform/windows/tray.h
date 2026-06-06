// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tray.h — System tray icon, menu, WndProc
 */
#pragma once
#include "globals.h"

#include <string>

void addTrayIcon(HWND hwnd);
void removeTrayIcon();
void showTrayMenu(HWND hwnd);
// Refresh the tray icon's hover tooltip to reflect current listening
// state + ports. Cheap (no-ops when text is unchanged); call from any
// thread whenever a status-affecting field changes.
void updateTrayTooltip();
// pairing.cpp listener: raise a native toast for a reverse-pairing request;
// clicking the toast opens an Accept/Reject dialog. Registered from WinMain.
void notifyPairRequestWindows(const std::string& deviceId);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
