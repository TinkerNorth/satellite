/*
 * vigem.h — ViGEmBus driver interaction
 */
#pragma once
#include "globals.h"

HANDLE openVigemBus();
bool   isVigemInstalled();
bool   pluginTarget(HANDLE bus, ULONG serial);
bool   submitReport(HANDLE bus, ULONG serial, const XUSB_REPORT& rpt);
void   unplugTarget(HANDLE bus, ULONG serial);

