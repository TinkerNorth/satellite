// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * vigem.h — Virtual gamepad driver presence check (macOS stub).
 *
 * Windows uses this file to declare the full ViGEm driver API. On macOS
 * there is no ViGEm equivalent yet, so only the presence check is exposed
 * here — it's the one symbol webserver.cpp references.
 *
 * Returns false on macOS until a DriverKit-based virtual HID backend ships.
 */
#pragma once
#include "globals.h"

bool isVigemInstalled();
