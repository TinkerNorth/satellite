// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tray.h — System tray icon, menu, WndProc
 */
#pragma once
#include "globals.h"

void addTrayIcon(HWND hwnd);
void removeTrayIcon();
void showTrayMenu(HWND hwnd);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
