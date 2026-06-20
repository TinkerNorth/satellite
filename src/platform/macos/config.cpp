// SPDX-License-Identifier: LGPL-3.0-or-later
#include "config.h"

#include "core/config_json.h"

#include <fcntl.h>
#include <mach-o/dyld.h>
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

static std::string appSupportDir() {
    std::string dir = homeDir() + "/Library/Application Support/satellite";
    mkdir((homeDir() + "/Library").c_str(), 0755);
    mkdir((homeDir() + "/Library/Application Support").c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    return dir;
}

std::string configPath() { return appSupportDir() + "/config.json"; }

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

// XML entity escaping for the launchd plist below — a path with '&' or '<' would
// otherwise produce a malformed plist.
static std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

static std::string launchAgentPath() {
    std::string dir = homeDir() + "/Library/LaunchAgents";
    mkdir(dir.c_str(), 0755);
    return dir + "/com.tinkernorth.satellite.plist";
}

void setAutoStart(bool enable) {
    std::string plist = launchAgentPath();
    if (enable) {
        std::string exe = getExeDir() + "/satellite";
        std::ofstream f(plist);
        if (!f.is_open()) return;
        f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
          << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
          << "<plist version=\"1.0\">\n"
          << "<dict>\n"
          << "  <key>Label</key><string>com.tinkernorth.satellite</string>\n"
          << "  <key>ProgramArguments</key>\n"
          << "  <array>\n"
          << "    <string>" << xmlEscape(exe) << "</string>\n"
          << "  </array>\n"
          << "  <key>RunAtLoad</key><true/>\n"
          << "  <key>KeepAlive</key><false/>\n"
          << "</dict>\n"
          << "</plist>\n";
        f.close();
        std::string cmd = "/bin/launchctl load -w " + plist + " >/dev/null 2>&1";
        (void)system(cmd.c_str());
    } else {
        std::string cmd = "/bin/launchctl unload -w " + plist + " >/dev/null 2>&1";
        (void)system(cmd.c_str());
        unlink(plist.c_str());
    }
}

bool getAutoStart() {
    struct stat st;
    return stat(launchAgentPath().c_str(), &st) == 0;
}

std::string getExeDir() {
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return ".";
    char resolved[PATH_MAX];
    const char* p = realpath(buf, resolved) != nullptr ? resolved : buf;
    std::string path(p);
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
