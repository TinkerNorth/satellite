/*
 * vigem.h — Virtual gamepad driver presence check (Linux).
 *
 * On Windows this probes the ViGEm bus driver. On Linux the analogous
 * question is "is /dev/uinput available and writable?" — the uinput
 * kernel module ships with every mainline kernel, so the check boils
 * down to file permissions / module load status.
 */
#pragma once
#include "globals.h"

bool isVigemInstalled();
