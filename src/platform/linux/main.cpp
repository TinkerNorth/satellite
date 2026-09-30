// SPDX-License-Identifier: LGPL-3.0-or-later
// Linux entry point / composition root. Mirrors the Windows and macOS mains.
#include "globals.h"
#include "config.h"
#include "crypto.h"
#include "gamepad_adapter.h"
#include "netlink_rejoin.h"
#include "tray.h"
#include "updater_adapter.h"

#include "net/receiver.h"
#include "net/webserver.h"
#include "net/discovery.h"
#include "net/mdns_responder.h"
#include "net/pairing.h"
#include "net/session_crypto.h"

#include "adapters/client_adapter.h"
#include "adapters/log_adapter.h"
#include "adapters/audio/opus_codec.h"

#include "core/session_service.h"
#include "core/update_service.h"

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <thread>

#include "adapters/crash_adapter.h"
#include "core/crash_reporting.h"

namespace crash = satellite::crash;

#ifdef SATELLITE_HAS_TRAY
#include <glib-unix.h>
#include <gtk/gtk.h>
#endif

// Force an mDNS multicast rejoin whenever the kernel reports address or link
// churn (suspend/resume, DHCP renew, cable replug). Linux counterpart of the
// Windows NotifyAddrChange + WM_POWERBROADCAST triggers: without it, a host
// whose interface bounced across sleep/wake but kept the same DHCP lease holds
// a dead multicast membership forever — the responder's periodic sweep only
// rejoins when the bound IP actually changed. A wake that renegotiates the
// link always emits RTM_NEWLINK/RTM_NEWADDR even when the address is reused.
static void netlinkWatcherThread() {
    using namespace std::chrono;
    auto lastSignal = steady_clock::time_point{};
    while (g_appRunning.load(std::memory_order_relaxed)) {
        const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
        if (fd < 0) {
            std::this_thread::sleep_for(seconds(2));
            continue;
        }
        struct sockaddr_nl addr{};
        addr.nl_family = AF_NETLINK;
        addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            std::this_thread::sleep_for(seconds(2));
            continue;
        }
        while (g_appRunning.load(std::memory_order_relaxed)) {
            struct pollfd pfd{fd, POLLIN, 0};
            const int rc = ::poll(&pfd, 1, 500);
            if (rc < 0) {
                if (errno == EINTR) continue;
                break; // rebuild the socket
            }
            if (rc == 0) continue;
            alignas(4) char buf[8192];
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break; // ENOBUFS after an event storm: rebuild and resync
            if (!netwatch::batchWantsRejoin(buf, static_cast<size_t>(n))) continue;
            // Collapse the NEWLINK/NEWADDR burst a single reconfiguration
            // emits; the responder does a full announce per forced rejoin.
            const auto now = steady_clock::now();
            if (lastSignal != steady_clock::time_point{} && now - lastSignal < seconds(2)) {
                continue;
            }
            lastSignal = now;
            requestMdnsRejoin();
        }
        ::close(fd);
    }
}

// Stops accepting: the servers close and every worker loop sees the flag.
static void stopServers() {
    g_appRunning = false;
    g_httpServer.stop();
    if (g_clientServer) g_clientServer->stop();
}

// Block SIGINT/SIGTERM so the headless loop can sigwait them on the main thread
// rather than default-terminating during a worker's blocking syscall (recvfrom,
// accept). SIGPIPE is blocked too; it fires on httplib mid-write disconnects.
static void installHeadlessSignalHandling(sigset_t& set) {
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

#ifdef SATELLITE_HAS_TRAY
// Bridge SIGINT/SIGTERM into the GTK main loop (g_unix_signal_add pipes the
// signal so this fires on the main loop, not the delivering thread).
static gboolean onTraySignal(gpointer) {
    stopServers();
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}
#endif

static bool startNetwork() {
    if (netInit()) return true;
    std::fprintf(stderr, "Failed to initialize network subsystem\n");
    return false;
}

static bool startCrypto() {
    if (sodiumInit()) return true;
    std::fprintf(stderr, "Failed to initialize libsodium\n");
    return false;
}

// The persisted config, with the autostart flag read from where the desktop
// keeps it rather than from the file.
static void loadConfigSeedingAutostart() {
    g_config = loadConfig();
    g_config.autoStart = getAutoStart();
}

// Read per frame, not cached: unlike the master switch these gate the wire
// rather than the persona, so flipping one reaches a stream already playing
// instead of waiting for a replug.
static ControllerAudioPolicy audioPolicyFromConfig() {
    std::lock_guard<std::mutex> lk(g_configMtx);
    return ControllerAudioPolicy{g_config.controllerAudioMic, g_config.controllerAudioSpeaker,
                                 g_config.controllerAudioHaptics};
}

static void persistConfig() {
    std::lock_guard<std::mutex> lk(g_configMtx);
    saveConfig(g_config);
}

// web/ in priority order: side-by-side (dev), FHS-from-prefix, manual sudo
// install, then package install; the dev location when none exists yet.
static std::string resolveWebDir() {
    const std::string exeDir = getExeDir();
    const std::string candidates[] = {
        exeDir + "/web",
        exeDir + "/../share/satellite/web",
        "/usr/local/share/satellite/web",
        "/usr/share/satellite/web",
    };
    for (const auto& c : candidates) {
        struct stat st;
        if (stat(c.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return c;
    }
    return exeDir + "/web";
}

// The service's threads, joined in the order they were started.
struct Workers {
    std::thread recv;
    std::thread admin;
    std::thread client;
    std::thread disc;
    std::thread mdns;
    std::thread netlink;
};

static Workers startWorkers(SessionService& svc, ClientAdapter& clientAdapter) {
    Workers w;
    w.recv = std::thread(receiverThread, std::ref(svc), std::ref(clientAdapter));
    w.admin = std::thread(adminHttpThread, std::ref(svc));
    w.client = std::thread(clientApiThread, std::ref(svc));
    w.disc = std::thread(discoveryThread);
    w.mdns = std::thread(mdnsResponderThread);
    w.netlink = std::thread(netlinkWatcherThread);
    return w;
}

static void joinWorkers(Workers& w) {
    w.recv.join();
    w.admin.join();
    w.client.join();
    w.disc.join();
    w.mdns.join();
    w.netlink.join();
}

// The tray-driven GTK loop on the main thread. False when the tray could not
// come up (no display server, GTK init failure, a build without
// SATELLITE_HAS_TRAY), in which case the headless loop runs instead.
static bool runTrayLoop() {
    if (!addTrayIcon()) return false;
#ifdef SATELLITE_HAS_TRAY
    // Ignore SIGPIPE (httplib disconnects); bridge SIGINT/SIGTERM to GTK.
    signal(SIGPIPE, SIG_IGN);
    g_unix_signal_add(SIGINT, onTraySignal, nullptr);
    g_unix_signal_add(SIGTERM, onTraySignal, nullptr);
    // Reverse-pairing: a dish request raises a native notification with
    // Accept/Reject so the operator never has to open the web UI.
    setPairRequestListener(notifyPairRequestLinux);
    gtk_main();
    removeTrayIcon();
    return true;
#else
    return false;
#endif
}

// The headless loop: the main thread waits for SIGINT or SIGTERM (SIGPIPE only
// wakes it), then stops the servers so the workers can join.
static void runHeadlessLoop() {
    sigset_t sigset;
    installHeadlessSignalHandling(sigset);
    int sig = 0;
    while (sigwait(&sigset, &sig) == 0 && sig == SIGPIPE) {}
    stopServers();
}

// No command line: everything is configured through the web UI and the
// config file, so the entry point takes none.
//
// Long on purpose: the composition root. Every collaborator lives on this
// stack in the order it must be constructed, and the shutdown below mirrors
// that order; a helper per step would move the lifetimes out of sight.
int main() {
    if (!startNetwork() || !startCrypto()) return 1;
    loadConfigSeedingAutostart();

    // Nothing else claims the fatal signals on Linux, so this is the only
    // crash recorder satellite has here. It still arms only behind the
    // operator's opt-in and a DSN this build actually carries.
    crash::init(g_config.crashReporting, crash::databaseDirFor(configPath()));

    GamepadAdapter gamepadAdapter;
    ClientAdapter clientAdapter;
    LogAdapter logAdapter;
    // See the Windows main for why the codec is injected rather than built
    // into the core. uinput has no audio endpoint to feed today, but the
    // service's audio paths are platform-neutral and stay wired the same way.
    satellite::audio::OpusCodecFactory audioCodecs;
    SessionService svc(gamepadAdapter, clientAdapter, logAdapter, deriveSessionKey, &audioCodecs);
    svc.setAudioPolicy(audioPolicyFromConfig);

    LinuxUpdaterAdapter updaterAdapter("TinkerNorth", "satellite");
    UpdateService updateService(updaterAdapter, logAdapter, g_config, g_configMtx);
    updateService.setPersistCallback(persistConfig);
    g_updateService = &updateService;
    g_webDir = resolveWebDir();
    updateService.start();

    Workers workers = startWorkers(svc, clientAdapter);
    std::fprintf(stderr, "%s running; web UI at http://localhost:%d\n", APP_TITLE,
                 g_config.webPort);

    if (!runTrayLoop()) runHeadlessLoop();

    updateService.stop();
    g_updateService = nullptr;
    joinWorkers(workers);

    svc.closeAllSessions();
    saveConfig(g_config);

    // Flush before exit; a pending envelope is lost if the transport never
    // gets to run.
    crash::shutdown();

    netShutdown();
    return 0;
}
