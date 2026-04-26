// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * vigem_stub.cpp — macOS stub for ViGEm driver probe.
 *
 * macOS has no signed virtual-gamepad bus driver shipped with this project,
 * so the probe always reports "not installed". The gamepad adapter surfaces
 * this to the SessionService, which then replies ACK_ERR_VIGEM_UNAVAIL to
 * clients attempting to add controllers.
 */
#include "vigem.h"

bool isVigemInstalled() { return false; }
