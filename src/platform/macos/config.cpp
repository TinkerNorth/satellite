// SPDX-License-Identifier: LGPL-3.0-or-later
#include "config.h"

#include <mach-o/dyld.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <climits>

// Control bytes (< 0x20) other than \n are \uXXXX-escaped: a raw \r or \t in a
// device name would be invalid JSON and corrupt the config file.
std::string jsonEscape(const std::string& s) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c < 0x20) {
            out += "\\u00";
            out += kHex[(c >> 4) & 0xF];
            out += kHex[c & 0xF];
        } else {
            out += ch;
        }
    }
    return out;
}

std::string jsonGetString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    auto colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return "";
    auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

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

    auto getInt = [&](const char* key) -> int {
        auto pos = content.find(std::string("\"") + key + "\"");
        if (pos == std::string::npos) return -1;
        auto colon = content.find(':', pos);
        if (colon == std::string::npos) return -1;
        return atoi(content.c_str() + colon + 1);
    };
    auto getBool = [&](const char* key) -> bool {
        auto pos = content.find(std::string("\"") + key + "\"");
        if (pos == std::string::npos) return false;
        auto colon = content.find(':', pos);
        if (colon == std::string::npos) return false;
        auto rest = content.substr(colon + 1, 10);
        return rest.find("true") != std::string::npos;
    };
    auto getBoolOpt = [&](const char* key, bool* out) {
        auto pos = content.find(std::string("\"") + key + "\"");
        if (pos == std::string::npos) return;
        *out = getBool(key);
    };
    auto getInt64 = [&](const char* key) -> int64_t {
        auto pos = content.find(std::string("\"") + key + "\"");
        if (pos == std::string::npos) return -1;
        auto colon = content.find(':', pos);
        if (colon == std::string::npos) return -1;
        return strtoll(content.c_str() + colon + 1, nullptr, 10);
    };

    int v = 0;
    v = getInt("udpPort");
    if (v > 0) cfg.udpPort = v;
    v = getInt("webPort");
    if (v > 0) cfg.webPort = v;
    v = getInt("pairPort");
    if (v > 0) cfg.pairPort = v;
    v = getInt("discPort");
    if (v > 0) cfg.discPort = v;
    // Absent on pre-1.6 configs; default (true) keeps the broadcast beacon on so
    // discovery doesn't silently regress.
    getBoolOpt("discoveryBroadcastEnabled", &cfg.discoveryBroadcastEnabled);
    cfg.autoStart = getBool("autoStart");

    // OTA update preferences (see core/update_service.h).
    std::string ch = jsonGetString(content, "updateChannel");
    if (!ch.empty()) cfg.updateChannel = ch;
    getBoolOpt("autoCheck", &cfg.autoCheck);
    getBoolOpt("autoDownload", &cfg.autoDownload);
    getBoolOpt("autoInstall", &cfg.autoInstall);
    int iv = getInt("updateCheckIntervalHours");
    if (iv > 0) cfg.updateCheckIntervalHours = iv;
    int64_t le = getInt64("lastCheckEpoch");
    if (le >= 0) cfg.lastCheckEpoch = le;
    cfg.lastSeenVersion = jsonGetString(content, "lastSeenVersion");
    cfg.skipVersion = jsonGetString(content, "skipVersion");

    auto arrStart = content.find("\"pairedDevices\"");
    if (arrStart != std::string::npos) {
        auto bracket = content.find('[', arrStart);
        auto bracketEnd = content.find(']', bracket);
        if (bracket != std::string::npos && bracketEnd != std::string::npos) {
            std::string arr = content.substr(bracket, bracketEnd - bracket + 1);
            size_t pos = 0;
            while (true) {
                auto objStart = arr.find('{', pos);
                if (objStart == std::string::npos) break;
                auto objEnd = arr.find('}', objStart);
                if (objEnd == std::string::npos) break;
                std::string obj = arr.substr(objStart, objEnd - objStart + 1);
                PairedDevice dev;
                dev.id = jsonGetString(obj, "id");
                dev.name = jsonGetString(obj, "name");
                dev.lastIP = jsonGetString(obj, "lastIP");
                dev.pairedAt = jsonGetString(obj, "pairedAt");
                dev.sharedKeyHex = jsonGetString(obj, "sharedKey");
                // Absent on pre-1.3 configs; touchpadModeFromName("") yields TOUCHPAD_MODE_DS4.
                dev.touchpadMode = touchpadModeFromName(jsonGetString(obj, "touchpadMode"));
                if (!dev.id.empty()) cfg.pairedDevices.push_back(dev);
                pos = objEnd + 1;
            }
        }
    }
    return cfg;
}

void saveConfig(const Config& cfg) {
    std::ofstream f(configPath());
    f << "{\n"
      << "  \"udpPort\": " << cfg.udpPort << ",\n"
      << "  \"webPort\": " << cfg.webPort << ",\n"
      << "  \"pairPort\": " << cfg.pairPort << ",\n"
      << "  \"discPort\": " << cfg.discPort << ",\n"
      << "  \"discoveryBroadcastEnabled\": " << (cfg.discoveryBroadcastEnabled ? "true" : "false")
      << ",\n"
      << "  \"autoStart\": " << (cfg.autoStart ? "true" : "false") << ",\n"
      << "  \"updateChannel\": \"" << jsonEscape(cfg.updateChannel) << "\",\n"
      << "  \"autoCheck\": " << (cfg.autoCheck ? "true" : "false") << ",\n"
      << "  \"autoDownload\": " << (cfg.autoDownload ? "true" : "false") << ",\n"
      << "  \"autoInstall\": " << (cfg.autoInstall ? "true" : "false") << ",\n"
      << "  \"updateCheckIntervalHours\": " << cfg.updateCheckIntervalHours << ",\n"
      << "  \"lastCheckEpoch\": " << cfg.lastCheckEpoch << ",\n"
      << "  \"lastSeenVersion\": \"" << jsonEscape(cfg.lastSeenVersion) << "\",\n"
      << "  \"skipVersion\": \"" << jsonEscape(cfg.skipVersion) << "\",\n"
      << "  \"pairedDevices\": [\n";
    for (size_t i = 0; i < cfg.pairedDevices.size(); i++) {
        const auto& d = cfg.pairedDevices[i];
        f << "    {\"id\":\"" << jsonEscape(d.id) << "\",\"name\":\"" << jsonEscape(d.name)
          << "\",\"lastIP\":\"" << jsonEscape(d.lastIP) << "\",\"pairedAt\":\""
          << jsonEscape(d.pairedAt) << "\",\"sharedKey\":\"" << jsonEscape(d.sharedKeyHex)
          << "\",\"touchpadMode\":\"" << touchpadModeName(d.touchpadMode) << "\"}";
        if (i + 1 < cfg.pairedDevices.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n}\n";
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
          << "    <string>" << jsonEscape(exe) << "</string>\n"
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
