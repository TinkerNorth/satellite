// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fake_vigem {

struct Tuning {
    int submitUs;
    int plugMs;
    int unplugMs;
};

void configure(const Tuning& tuning);
void arm();
uint64_t submitCalls();

} // namespace fake_vigem
