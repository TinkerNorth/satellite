/*
 * tray.h — System tray icon, menu, WndProc
 */
#pragma once
#include "globals.h"

void addTrayIcon(HWND hwnd);
void removeTrayIcon();
void showTrayMenu(HWND hwnd);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
