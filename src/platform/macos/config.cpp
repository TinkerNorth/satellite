// SPDX-License-Identifier: LGPL-3.0-or-later
#include "config.h"

#include "config_posix.h"

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstring>

extern char** environ;

static std::string appSupportDir() {
    std::string dir = homeDir() + "/Library/Application Support/satellite";
    mkdir((homeDir() + "/Library").c_str(), 0755);
    mkdir((homeDir() + "/Library/Application Support").c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    return dir;
}

std::string configPath() { return appSupportDir() + "/config.json"; }

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
    // Create each level like appSupportDir() does: ~/Library always exists on
    // a real profile, but a redirected $HOME (tests) starts empty.
    mkdir((homeDir() + "/Library").c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    return dir + "/com.tinkernorth.satellite.plist";
}

// Registers/unregisters the LaunchAgent with the user's launchd. Elided from
// test builds: the platform suite redirects HOME into a tmpdir to exercise the
// plist contract hermetically, and `launchctl load -w` would still mutate the
// REAL launchd override database (keyed by label, not by path).
#ifdef SATELLITE_BUILD_TESTS
static void launchctlSetLoaded(bool /*load*/, const std::string& /*plist*/) {}
#else
static void launchctlSetLoaded(bool load, const std::string& plist) {
    // posix_spawn rather than system(): no shell, so the plist path is an
    // argument and never something to quote, and the exit status is ours to
    // read. launchctl's own chatter still goes to /dev/null.
    const char* verb = load ? "load" : "unload";
    const char* argv[] = {"/bin/launchctl", verb, "-w", plist.c_str(), nullptr};
    const std::string what = std::string("launchctl ") + verb + " -w " + plist;

    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0) {
        logMsg(LogLevel::WARN, "config", what + " could not start: " + std::strerror(rc));
        return;
    }
    pid_t pid = 0;
    rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (rc == 0) {
        rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    if (rc == 0) {
        rc = posix_spawn(&pid, argv[0], &actions, nullptr, const_cast<char* const*>(argv), environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        logMsg(LogLevel::WARN, "config", what + " could not start: " + std::strerror(rc));
        return;
    }

    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        const int err = errno;
        logMsg(LogLevel::WARN, "config", what + ": waitpid failed: " + std::strerror(err));
    } else if (!WIFEXITED(status)) {
        logMsg(LogLevel::WARN, "config", what + " was terminated by a signal");
    } else if (WEXITSTATUS(status) != 0) {
        logMsg(LogLevel::WARN, "config",
               what + " failed with exit status " + std::to_string(WEXITSTATUS(status)));
    }
}
#endif

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
        launchctlSetLoaded(true, plist);
    } else {
        launchctlSetLoaded(false, plist);
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
