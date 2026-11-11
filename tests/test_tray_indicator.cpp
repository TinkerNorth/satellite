// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/platform/linux/tray_indicator.h"

#include <cstring>
#include <iostream>

#include "test_util.h"

static void testNewStatusIndicatorCarriesItsIdentity() {
    TEST("newStatusIndicator: id, icon and application-status category are set");
    AppIndicator* const indicator = newStatusIndicator("satellite-test", "input-gaming");
    EXPECT(indicator != nullptr);
    EXPECT_EQ(std::strcmp(app_indicator_get_id(indicator), "satellite-test"), 0);
    EXPECT_EQ(std::strcmp(app_indicator_get_icon(indicator), "input-gaming"), 0);
    EXPECT_EQ(static_cast<int>(app_indicator_get_category(indicator)),
              static_cast<int>(APP_INDICATOR_CATEGORY_APPLICATION_STATUS));
    g_object_unref(indicator);
}

static void testNewStatusIndicatorStartsPassive() {
    TEST("newStatusIndicator: the indicator is hidden until the caller activates it");
    AppIndicator* const indicator = newStatusIndicator("satellite-test", "input-gaming");
    EXPECT_EQ(static_cast<int>(app_indicator_get_status(indicator)),
              static_cast<int>(APP_INDICATOR_STATUS_PASSIVE));
    g_object_unref(indicator);
}

int main() {
    std::cout << "Running tray indicator tests...\n\n";

    testNewStatusIndicatorCarriesItsIdentity();
    testNewStatusIndicatorStartsPassive();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    std::cout << "  STATUS: " << (g_fail == 0 ? "ALL PASSED" : "FAILED") << "\n";
    return g_fail == 0 ? 0 : 1;
}
