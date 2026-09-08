// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <string>
#include <sys/types.h>

namespace satellite::update {

inline std::string buildSwapScript(pid_t pid, const std::string& src, const std::string& dst,
                                   int waitTicks = 60) {
    std::string script;
    script += "#!/bin/bash\n";
    script += "set -e\n";
    script += "PID=" + std::to_string(pid) + "\n";
    script += "SRC=\"" + src + "\"\n";
    script += "DST=\"" + dst + "\"\n";
    script += "for i in $(seq 1 " + std::to_string(waitTicks) + "); do\n";
    script += "  if ! kill -0 \"$PID\" 2>/dev/null; then break; fi\n";
    script += "  sleep 0.5\n";
    script += "done\n";
    script += "if kill -0 \"$PID\" 2>/dev/null; then\n";
    script += "  rm -f -- \"$0\"\n";
    script += "  exit 1\n";
    script += "fi\n";
    // Same-fs mv is atomic. Keep a .old copy so the user can roll back if the
    // new binary crashes on startup.
    script += "if [ -f \"$DST\" ]; then mv -f \"$DST\" \"$DST.old\" || true; fi\n";
    script += "mv -f \"$SRC\" \"$DST\"\n";
    script += "chmod +x \"$DST\"\n";
    // setsid so the relaunched AppImage survives our exit.
    script += "setsid \"$DST\" >/dev/null 2>&1 &\n";
    script += "rm -f -- \"$0\"\n";
    return script;
}

} // namespace satellite::update
