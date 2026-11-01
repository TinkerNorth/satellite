// SPDX-License-Identifier: LGPL-3.0-or-later
#include "file_log.h"

#include "core/log_writer.h"

#include <strsafe.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace lifecycle {

namespace {

constexpr size_t kMaxLogFileBytes = 5 * 1024 * 1024;
constexpr int kLogRetentionDays = 7;
constexpr int kMaxRotatedSuffix = 100;
constexpr size_t kLogExtensionLength = 4;
constexpr auto kReopenRetry = std::chrono::seconds(1);

std::atomic<HANDLE> g_openLog{nullptr};

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void deleteOlderThan(const std::wstring& dir, const wchar_t* ext, int days) {
    const std::wstring pattern = dir + L"\\*" + ext;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    FILETIME ftNow;
    GetSystemTimeAsFileTime(&ftNow);
    const ULONGLONG now =
        (static_cast<ULONGLONG>(ftNow.dwHighDateTime) << 32) | ftNow.dwLowDateTime;
    const ULONGLONG kFtPerDay = 10000000ULL * 60ULL * 60ULL * 24ULL;
    const ULONGLONG cutoff = now - static_cast<ULONGLONG>(days) * kFtPerDay;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const ULONGLONG t = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                            fd.ftLastWriteTime.dwLowDateTime;
        if (t < cutoff) {
            const std::wstring full = dir + L"\\" + fd.cFileName;
            DeleteFileW(full.c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::tm localNow() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    return tm;
}

int dayKey(const std::tm& tm) {
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

class FileLogSink : public satellite::ILogSink {
  public:
    explicit FileLogSink(std::wstring dir) : dir_(std::move(dir)) {}
    ~FileLogSink() override { close(); }

    FileLogSink(const FileLogSink&) = delete;
    FileLogSink& operator=(const FileLogSink&) = delete;

    bool write(std::string_view batch) override {
        if (file_ == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0;
        const BOOL ok =
            WriteFile(file_, batch.data(), static_cast<DWORD>(batch.size()), &wrote, nullptr);
        bytes_ += wrote;
        return ok != 0 && wrote == batch.size();
    }

    bool flush() override { return file_ != INVALID_HANDLE_VALUE && FlushFileBuffers(file_) != 0; }

    bool rotateIfDue() override {
        const std::tm today = localNow();
        const int key = dayKey(today);
        if (file_ == INVALID_HANDLE_VALUE) {
            openFor(key, today);
            return false;
        }
        if (bytes_ >= kMaxLogFileBytes) return rotateOversized();
        if (key != day_) return openFor(key, today);
        return false;
    }

    void purge() override {
        if (!dir_.empty()) deleteOlderThan(dir_, L".log", kLogRetentionDays);
    }

  private:
    bool openFor(int key, const std::tm& day) {
        if (dir_.empty()) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now < retryAt_) return false;
        wchar_t name[32];
        StringCchPrintfW(name, ARRAYSIZE(name), L"satellite-%04d%02d%02d.log", day.tm_year + 1900,
                         day.tm_mon + 1, day.tm_mday);
        if (open(dir_ + L"\\" + name, key)) return true;
        retryAt_ = now + kReopenRetry;
        return false;
    }

    bool open(const std::wstring& path, int key) {
        const HANDLE file =
            CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        bytes_ = GetFileSizeEx(file, &size) ? static_cast<size_t>(size.QuadPart) : 0;
        const HANDLE previous = file_;
        g_openLog.store(file, std::memory_order_release);
        file_ = file;
        path_ = path;
        day_ = key;
        if (previous != INVALID_HANDLE_VALUE) CloseHandle(previous);
        return true;
    }

    bool rotateOversized() {
        const std::wstring current = path_;
        const std::wstring stem = current.substr(0, current.size() - kLogExtensionLength);
        for (int n = 1; n < kMaxRotatedSuffix; ++n) {
            wchar_t tail[16];
            StringCchPrintfW(tail, ARRAYSIZE(tail), L".%d.log", n);
            const std::wstring rotated = stem + tail;
            if (MoveFileExW(current.c_str(), rotated.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                return open(current, day_);
            }
        }
        bytes_ = 0;
        return false;
    }

    void close() {
        if (file_ == INVALID_HANDLE_VALUE) return;
        HANDLE published = file_;
        g_openLog.compare_exchange_strong(published, nullptr, std::memory_order_acq_rel);
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }

    const std::wstring dir_;
    std::wstring path_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    int day_ = 0;
    size_t bytes_ = 0;
    std::chrono::steady_clock::time_point retryAt_{};
};

} // namespace

void startFileLogger(const std::string& dir) {
    if (logWriter().running()) return;
    logWriter().start(std::make_unique<FileLogSink>(utf8ToWide(dir)));
}

void stopFileLogger() { logWriter().stop(); }

bool flushFileLog(DWORD timeoutMs) {
    return logWriter().flush(std::chrono::milliseconds(timeoutMs));
}

bool writeLogLineDirect(const char* line, size_t length) {
    const HANDLE file = g_openLog.load(std::memory_order_acquire);
    if (file == nullptr) return false;
    DWORD wrote = 0;
    const bool written =
        WriteFile(file, line, static_cast<DWORD>(length), &wrote, nullptr) != 0 && wrote == length;
    const bool flushed = FlushFileBuffers(file) != 0;
    return written && flushed;
}

} // namespace lifecycle
