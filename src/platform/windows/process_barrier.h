// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <winsock2.h>
#include <windows.h>

namespace processbarrier {

inline void flushWriteBuffers() { FlushProcessWriteBuffers(); }

} // namespace processbarrier
