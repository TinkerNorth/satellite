// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/event_broadcaster.h"
#include "httplib.h"

#include <memory>
#include <string>

namespace satellite {

void serveEventStreams(httplib::Server& server, const std::string& path,
                       std::shared_ptr<EventBroadcaster> streams);

} // namespace satellite
