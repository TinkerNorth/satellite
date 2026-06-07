// SPDX-License-Identifier: LGPL-3.0-or-later

// Also consumed by windres (gcc -E on .rc files), so keep it free of any
// non-preprocessor constructs. VERSION BUMPS: change all three macros below and
// mirror them in the top-level /VERSION; CI `version-consistency` fails on drift.
#ifndef SATELLITE_CORE_VERSION_H
#define SATELLITE_CORE_VERSION_H

#define SATELLITE_VERSION_MAJOR 1
#define SATELLITE_VERSION_MINOR 0
#define SATELLITE_VERSION_PATCH 0

// Wire string (matches /VERSION). Used by /api/version, the GitHub API
// User-Agent, and version comparison.
#define SATELLITE_VERSION_STRING "1.0.0"

// Comma form for Win32 VS_VERSION_INFO; trailing zero is the unused build slot.
#define SATELLITE_VERSION_COMMA 1, 0, 0, 0

// Dotted-quad form for the VS_VERSION_INFO string properties.
#define SATELLITE_VERSION_DOTTED "1.0.0.0"

#endif // SATELLITE_CORE_VERSION_H
