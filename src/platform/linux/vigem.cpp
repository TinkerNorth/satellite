/*
 * vigem.cpp — Linux uinput probe.
 *
 * Returns true iff /dev/uinput exists and the current user has write access.
 * Typical remediation on failure: load the uinput module (`sudo modprobe uinput`)
 * and add the user to a group with write access (commonly `input`, or via a
 * udev rule granting the user rw on /dev/uinput).
 */
#include "vigem.h"

#include <unistd.h>

bool isVigemInstalled() { return ::access("/dev/uinput", W_OK) == 0; }
