// SPDX-License-Identifier: LGPL-3.0-or-later
// Pure classifier for NETLINK_ROUTE notification batches. The netlink watcher
// in main.cpp subscribes to RTMGRP_LINK | RTMGRP_IPV4_IFADDR and hands each
// recv() batch here; a `true` verdict means the host's addressing or link
// state moved in a way that can silently invalidate mDNS multicast group
// membership (suspend/resume, DHCP renew, cable replug), so the responder
// must force-rejoin. Header-only and side-effect free so test_linux_platform
// can drive it with synthetic buffers.
#pragma once

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <climits>
#include <cstddef>

namespace netwatch {

// True when the batch carries at least one address or link lifecycle message.
// Iteration is bounded by NLMSG_OK, which rejects truncated headers and
// lengths that overrun the buffer; `remaining` stays an int (like iproute2)
// because NLMSG_NEXT's subtraction may briefly go negative on the final
// aligned message and must not wrap.
inline bool batchWantsRejoin(const void* buf, size_t len) {
    if (buf == nullptr || len > static_cast<size_t>(INT_MAX)) return false;
    const struct nlmsghdr* nh = static_cast<const struct nlmsghdr*>(buf);
    for (int remaining = static_cast<int>(len); NLMSG_OK(nh, remaining);
         nh = NLMSG_NEXT(nh, remaining)) {
        switch (nh->nlmsg_type) {
        case RTM_NEWADDR:
        case RTM_DELADDR:
        case RTM_NEWLINK:
        case RTM_DELLINK:
            return true;
        case NLMSG_DONE:
            return false;
        default:
            break;
        }
    }
    return false;
}

} // namespace netwatch
