// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/hot_table.h"

#include <linux/membarrier.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace processbarrier {

inline void privateExpedited() { syscall(__NR_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0); }

inline ProcessBarrierFn registerPrivateExpedited() {
    if (syscall(__NR_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) != 0) {
        return nullptr;
    }
    return privateExpedited;
}

} // namespace processbarrier
