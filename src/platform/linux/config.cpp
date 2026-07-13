// SPDX-License-Identifier: LGPL-3.0-or-later
#include "config.h"

#include "config_posix.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <climits>

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
