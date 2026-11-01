// SPDX-License-Identifier: LGPL-3.0-or-later
#include "file_log.h"

#include "globals.h"
#include "config_posix.h"

#include "core/log_writer.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace lifecycle {

namespace {

constexpr size_t kMaxLogFileBytes = 5 * 1024 * 1024;
constexpr int kLogRetentionDays = 7;
constexpr int kMaxRotatedSuffix = 100;
constexpr size_t kLogExtensionLength = 4;
constexpr auto kReopenRetry = std::chrono::seconds(1);
constexpr auto kRetention = std::chrono::hours(24 * kLogRetentionDays);

std::tm localNow() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    return tm;
}

int dayKey(const std::tm& tm) {
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

std::string logFileName(const std::tm& day) {
    char name[64];
    std::snprintf(name, sizeof(name), "satellite-%04d%02d%02d.log", day.tm_year + 1900,
                  day.tm_mon + 1, day.tm_mday);
    return name;
}

void deleteOlderThan(const std::string& dir) {
    const auto cutoff = std::filesystem::file_time_type::clock::now() - kRetention;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (it->path().extension() != ".log" || !it->is_regular_file(entryEc)) continue;
        const auto written = it->last_write_time(entryEc);
        if (!entryEc && written < cutoff) std::filesystem::remove(it->path(), entryEc);
    }
}

class FileLogSink : public satellite::ILogSink {
  public:
    explicit FileLogSink(std::string dir) : dir_(std::move(dir)) {}
    ~FileLogSink() override {
        if (fd_ >= 0) ::close(fd_);
    }

    FileLogSink(const FileLogSink&) = delete;
    FileLogSink& operator=(const FileLogSink&) = delete;

    bool write(std::string_view batch) override {
        if (fd_ < 0) return false;
        size_t done = 0;
        while (done < batch.size()) {
            const ssize_t n = ::write(fd_, batch.data() + done, batch.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            done += static_cast<size_t>(n);
            bytes_ += static_cast<size_t>(n);
        }
        return true;
    }

    bool flush() override { return fd_ >= 0 && ::fsync(fd_) == 0; }

    bool rotateIfDue() override {
        const std::tm today = localNow();
        const int key = dayKey(today);
        if (fd_ < 0) {
            openFor(key, today);
            return false;
        }
        if (bytes_ >= kMaxLogFileBytes) return rotateOversized();
        if (key != day_) return openFor(key, today);
        return false;
    }

    void purge() override {
        if (!dir_.empty()) deleteOlderThan(dir_);
    }

  private:
    bool openFor(int key, const std::tm& day) {
        if (dir_.empty()) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now < retryAt_) return false;
        if (openPath(dir_ + "/" + logFileName(day), key)) return true;
        retryAt_ = now + kReopenRetry;
        return false;
    }

    bool openPath(const std::string& path, int key) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) return false;
        struct stat st{};
        bytes_ = ::fstat(fd, &st) == 0 ? static_cast<size_t>(st.st_size) : 0;
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
        path_ = path;
        day_ = key;
        return true;
    }

    bool rotateOversized() {
        const std::string current = path_;
        const std::string stem = current.substr(0, current.size() - kLogExtensionLength);
        for (int n = 1; n < kMaxRotatedSuffix; ++n) {
            std::error_code ec;
            std::filesystem::rename(current, stem + "." + std::to_string(n) + ".log", ec);
            if (!ec) return openPath(current, day_);
        }
        bytes_ = 0;
        return false;
    }

    const std::string dir_;
    std::string path_;
    int fd_ = -1;
    int day_ = 0;
    size_t bytes_ = 0;
    std::chrono::steady_clock::time_point retryAt_{};
};

} // namespace

std::string logDir() {
#ifdef __APPLE__
    const std::filesystem::path dir =
        std::filesystem::path(homeDir()) / "Library" / "Logs" / "Satellite";
#else
    const char* state = std::getenv("XDG_STATE_HOME");
    const std::filesystem::path base = state != nullptr && state[0] == '/'
                                           ? std::filesystem::path(state)
                                           : std::filesystem::path(homeDir()) / ".local" / "state";
    const std::filesystem::path dir = base / "satellite" / "logs";
#endif
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

void startFileLogger(const std::string& dir) {
    if (logWriter().running()) return;
    logWriter().start(std::make_unique<FileLogSink>(dir));
}

void stopFileLogger() { logWriter().stop(); }

bool flushFileLog(unsigned timeoutMs) {
    return logWriter().flush(std::chrono::milliseconds(timeoutMs));
}

} // namespace lifecycle
