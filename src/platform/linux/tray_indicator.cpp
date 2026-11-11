// SPDX-License-Identifier: LGPL-3.0-or-later
#include "tray_indicator.h"

// The library deprecates app_indicator_new (libayatana-appindicator >= 0.5.94)
// without a replacement in this header; its construct-only properties, which
// that function sets itself, are the supported way in.
AppIndicator* newStatusIndicator(const char* id, const char* iconName) {
    const gpointer indicator = g_object_new(APP_INDICATOR_TYPE, "id", id, "category",
                                            "ApplicationStatus", "icon-name", iconName, nullptr);
    return APP_INDICATOR(indicator);
}
