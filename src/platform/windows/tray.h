// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "globals.h"

#include <string>

void addTrayIcon(HWND hwnd);
void removeTrayIcon();
void showTrayMenu(HWND hwnd);
// Thread-safe; no-ops when the tooltip text is unchanged.
void updateTrayTooltip();
// pairing.cpp listener: toast a reverse-pairing request. Registered from WinMain.
void notifyPairRequestWindows(const std::string& deviceId);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
