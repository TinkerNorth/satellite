// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <libayatana-appindicator/app-indicator.h>

// An application-status indicator named `id` showing the themed `iconName`.
AppIndicator* newStatusIndicator(const char* id, const char* iconName);
