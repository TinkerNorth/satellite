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
// The walk is NLMSG_OK / NLMSG_NEXT's, in size_t: a header must fit in what is
// left, its length must cover a header and stay inside the batch, and the next
// message starts at the aligned length, so the walk ends once that steps past
// the end. (The macros compare the kernel's unsigned length with an int.)
inline bool batchWantsRejoin(const void* buf, size_t len) {
    if (buf == nullptr || len > static_cast<size_t>(INT_MAX)) return false;
    const auto* at = static_cast<const unsigned char*>(buf);
    size_t remaining = len;
    while (remaining >= sizeof(struct nlmsghdr)) {
        const auto* nh = reinterpret_cast<const struct nlmsghdr*>(at);
        const size_t messageLen = nh->nlmsg_len;
        if (messageLen < sizeof(struct nlmsghdr) || messageLen > remaining) return false;
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
        const size_t step = NLMSG_ALIGN(messageLen);
        if (step >= remaining) return false;
        at += step;
        remaining -= step;
    }
    return false;
}

} // namespace netwatch
