/*
 * vigem.h — ViGEmBus driver interaction
 */
#pragma once
#include "globals.h"

HANDLE openVigemBus();
bool isVigemInstalled();
bool pluginTarget(HANDLE bus, ULONG serial);
bool pluginTargetDS4(HANDLE bus, ULONG serial);
bool submitReport(HANDLE bus, ULONG serial, const XUSB_REPORT& rpt);
bool submitReportFast(HANDLE bus, ULONG serial, const XUSB_REPORT& rpt, HANDLE event);
bool submitReportDS4Fast(HANDLE bus, ULONG serial, const DS4_REPORT& rpt, HANDLE event);
void unplugTarget(HANDLE bus, ULONG serial);
