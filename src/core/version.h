// SPDX-License-Identifier: LGPL-3.0-or-later

// Also consumed by windres, so keep it free of non-preprocessor constructs.
// VERSION BUMPS: change all three macros below and mirror them in /VERSION;
// CI `version-consistency` fails on drift.
#ifndef SATELLITE_CORE_VERSION_H
#define SATELLITE_CORE_VERSION_H

#define SATELLITE_VERSION_MAJOR 2
#define SATELLITE_VERSION_MINOR 0
#define SATELLITE_VERSION_PATCH 3

// Wire string (matches /VERSION).
#define SATELLITE_VERSION_STRING "2.0.3"

// Comma form for Win32 VS_VERSION_INFO; trailing zero is the unused build slot.
#define SATELLITE_VERSION_COMMA 2, 0, 3, 0

#define SATELLITE_VERSION_DOTTED "2.0.3.0"

#endif // SATELLITE_CORE_VERSION_H
