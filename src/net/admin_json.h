// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

class SessionService;

std::string buildConnectionsJson(const SessionService& svc);
std::string buildDevicesJson(const SessionService& svc);
std::string buildLogsJson(uint64_t since);
