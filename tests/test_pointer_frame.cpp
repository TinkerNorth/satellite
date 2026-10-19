// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/pointer_frame.h"
#include "test_util.h"

using satellite::planPointerFrame;
using satellite::pointerButtonsAfter;
using satellite::PointerEventKind;

static void test_refusedReleaseCanRetry() {
    TEST("pointer: refused button releases remain pending for retry");
    const MouseButtons held{true, true, true};
    const auto frame = planPointerFrame(held, MouseButtons{}, 0, 0, 0);
    const MouseButtons after = pointerButtonsAfter(frame, 0);
    EXPECT(after.left);
    EXPECT(after.right);
    EXPECT(after.middle);
    const auto retry = planPointerFrame(after, MouseButtons{}, 0, 0, 0);
    EXPECT_EQ(retry.count, size_t{3});
}

static void test_refusedPressCanRetry() {
    TEST("pointer: refused button presses remain pending for retry");
    const MouseButtons wanted{true, true, true};
    const auto frame = planPointerFrame(MouseButtons{}, wanted, 0, 0, 0);
    const MouseButtons after = pointerButtonsAfter(frame, 0);
    EXPECT(!after.left);
    EXPECT(!after.right);
    EXPECT(!after.middle);
    const auto retry = planPointerFrame(after, wanted, 0, 0, 0);
    EXPECT_EQ(retry.count, size_t{3});
}

static void test_partialBatchCommitsOnlyInsertedEvents() {
    TEST("pointer: partial insertion commits only the accepted button edges");
    const MouseButtons held{true, true, true};
    const auto frame = planPointerFrame(held, MouseButtons{}, 10, -20, 120);
    EXPECT_EQ(frame.count, size_t{5});
    EXPECT(frame.events[0].kind == PointerEventKind::Move);
    EXPECT_EQ(frame.events[0].x, 10);
    EXPECT_EQ(frame.events[0].y, -20);
    EXPECT(frame.events[4].kind == PointerEventKind::Wheel);
    EXPECT_EQ(frame.events[4].y, 120);
    for (size_t accepted = 0; accepted <= frame.count; accepted++) {
        const MouseButtons after = pointerButtonsAfter(frame, accepted);
        EXPECT_EQ(after.left, accepted < 2);
        EXPECT_EQ(after.right, accepted < 3);
        EXPECT_EQ(after.middle, accepted < 4);
    }
}

static void test_successCoalescesAndIdleStaysEmpty() {
    TEST("pointer: accepted levels emit no repeated button edges");
    const MouseButtons wanted{true, false, true};
    const auto frame = planPointerFrame(MouseButtons{}, wanted, 0, 0, 0);
    const MouseButtons after = pointerButtonsAfter(frame, frame.count);
    EXPECT(after.left);
    EXPECT(!after.right);
    EXPECT(after.middle);
    const auto idle = planPointerFrame(after, wanted, 0, 0, 0);
    EXPECT_EQ(idle.count, size_t{0});
    const auto unchanged = pointerButtonsAfter(idle, 0);
    EXPECT(unchanged.left);
    EXPECT(unchanged.middle);
}

int main() {
    test_refusedReleaseCanRetry();
    test_refusedPressCanRetry();
    test_partialBatchCommitsOnlyInsertedEvents();
    test_successCoalescesAndIdleStaysEmpty();
    std::cout << "Passed: " << g_pass << ", failed: " << g_fail << "\n";
    return g_fail == 0 ? 0 : 1;
}
