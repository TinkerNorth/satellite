// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/network_info.h"

#include <cstdint>
#include <string>
#include <vector>

std::vector<LocalInterface> enumerateInterfaces(bool withCategory);
bool resolveBoundIPv4(const std::string& selectedName, uint32_t& ipv4NetworkOrder);
bool allowPublicFirewall();
