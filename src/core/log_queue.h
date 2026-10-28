// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/log_record.h"
#include "core/mpsc_queue.h"

#include <cstddef>

namespace satellite {

inline constexpr size_t LOG_QUEUE_CAPACITY = 4096;

template <size_t Capacity> using BasicLogQueue = MpscQueue<LogRecord, Capacity>;

using LogQueue = BasicLogQueue<LOG_QUEUE_CAPACITY>;

} // namespace satellite
