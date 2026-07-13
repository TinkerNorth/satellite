// SPDX-License-Identifier: LGPL-3.0-or-later
#include "config.h"

#include "config_posix.h"

#include <fcntl.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <ctime>
#include <string>

std::string homeDir() {
    const char* h = getenv("HOME");
    if (h != nullptr && *h != 0) return h;
    struct passwd* pw = getpwuid(getuid());
    return (pw != nullptr && pw->pw_dir != nullptr) ? pw->pw_dir : "/tmp";
}

bool atomicWriteFile(const std::string& path, const std::string& bytes) {
    std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    bool ok = true;
    size_t off = 0;
    while (off < bytes.size()) {
        ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        off += static_cast<size_t>(n);
    }
    if (ok) ok = ::fsync(fd) == 0;
    if (::close(fd) != 0) ok = false;

    if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

std::string getCurrentDate() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    struct tm tm{};
    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}
