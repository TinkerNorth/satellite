// SPDX-License-Identifier: LGPL-3.0-or-later
#include "config.h"

#include "core/config_json.h"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <climits>

static std::string homeDir() {
    const char* h = getenv("HOME");
    if (h != nullptr && *h != 0) return h;
    struct passwd* pw = getpwuid(getuid());
    return (pw != nullptr && pw->pw_dir != nullptr) ? pw->pw_dir : "/tmp";
}

static std::string xdgConfigHome() {
    const char* x = getenv("XDG_CONFIG_HOME");
    if (x != nullptr && *x != 0) return x;
    return homeDir() + "/.config";
}

static std::string appConfigDir() {
    std::string base = xdgConfigHome();
    mkdir(base.c_str(), 0755);
    std::string dir = base + "/satellite";
    mkdir(dir.c_str(), 0755);
    return dir;
}

std::string configPath() { return appConfigDir() + "/config.json"; }

Config loadConfig() {
    Config cfg;
    std::ifstream f(configPath());
    if (!f.is_open()) return cfg;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    satellite::parseConfigInto(content, cfg);
    return cfg;
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

void saveConfig(const Config& cfg) {
    atomicWriteFile(configPath(), satellite::serializeConfig(cfg));
}

static std::string autostartDir() {
    std::string dir = xdgConfigHome() + "/autostart";
    mkdir(dir.c_str(), 0755);
    return dir;
}

static std::string autostartPath() { return autostartDir() + "/satellite.desktop"; }

void setAutoStart(bool enable) {
    std::string path = autostartPath();
    if (enable) {
        std::string exe = getExeDir() + "/satellite";
        std::ofstream f(path);
        if (!f.is_open()) return;
        f << "[Desktop Entry]\n"
          << "Type=Application\n"
          << "Name=" << APP_TITLE << "\n"
          << "Exec=" << exe << "\n"
          << "X-GNOME-Autostart-enabled=true\n"
          << "NoDisplay=false\n"
          << "Terminal=false\n";
    } else {
        unlink(path.c_str());
    }
}

bool getAutoStart() {
    struct stat st;
    return stat(autostartPath().c_str(), &st) == 0;
}

std::string getExeDir() {
    char buf[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    std::string path(buf);
    auto pos = path.find_last_of('/');
    return (pos != std::string::npos) ? path.substr(0, pos) : ".";
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
