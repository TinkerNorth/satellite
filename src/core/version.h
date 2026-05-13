// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * core/version.h — Single source of truth for the Satellite binary version.
 *
 * This file is consumed by both C++ translation units AND windres (which
 * preprocesses .rc files through gcc -E), so it must stay free of any
 * non-preprocessor constructs.
 *
 * VERSION BUMPS: change all three places below, then mirror them in the
 * top-level VERSION file. The CI workflow `version-consistency` will fail
 * if /VERSION drifts from these macros.
 *
 *   /VERSION                                — used by CMake (MACOSX_BUNDLE_*,
 *                                             CPACK_PACKAGE_VERSION) and by
 *                                             installer.iss (FileOpen at
 *                                             preprocess time).
 *   src/core/version.h (this file)          — used by C++ + satellite.rc.
 *
 * The macros below intentionally avoid string concatenation tricks so the
 * file remains readable from a diff.
 */
#ifndef SATELLITE_CORE_VERSION_H
#define SATELLITE_CORE_VERSION_H

#define SATELLITE_VERSION_MAJOR  1
#define SATELLITE_VERSION_MINOR  0
#define SATELLITE_VERSION_PATCH  0

// Wire string (matches /VERSION). Used by /api/version, the User-Agent
// header sent to GitHub's release API, and the parsed-version comparison.
#define SATELLITE_VERSION_STRING "1.0.0"

// Comma form for Win32 VS_VERSION_INFO.FILEVERSION / .PRODUCTVERSION.
// The trailing zero is a Win32 "build" slot we don't currently use.
#define SATELLITE_VERSION_COMMA  1,0,0,0

// Dotted-quad form for the VS_VERSION_INFO "FileVersion" / "ProductVersion"
// string properties (the ones shown in Explorer → Properties → Details).
#define SATELLITE_VERSION_DOTTED "1.0.0.0"

#endif // SATELLITE_CORE_VERSION_H
