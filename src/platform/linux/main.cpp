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
    g_appRunning = false;
    g_httpServer.stop();
    if (g_clientServer) g_clientServer->stop();
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}
#endif

int main(int argc, const char* argv[]) {
    (void)argc;
    (void)argv;

    if (!netInit()) {
        std::fprintf(stderr, "Failed to initialize network subsystem\n");
        return 1;
    }

    if (!sodiumInit()) {
        std::fprintf(stderr, "Failed to initialize libsodium\n");
        return 1;
    }

    g_config = loadConfig();
    g_config.autoStart = getAutoStart();

    GamepadAdapter gamepadAdapter;
    ClientAdapter clientAdapter;
    LogAdapter logAdapter;
    SessionService svc(gamepadAdapter, clientAdapter, logAdapter, deriveSessionKey);

    LinuxUpdaterAdapter updaterAdapter("TinkerNorth", "satellite");
    UpdateService updateService(updaterAdapter, logAdapter, g_config, g_configMtx);
    updateService.setPersistCallback([] {
        std::lock_guard<std::mutex> lk(g_configMtx);
        saveConfig(g_config);
    });
    g_updateService = &updateService;

    // Resolve web/ in priority order: side-by-side (dev), FHS-from-prefix,
    // manual sudo install, then package install.
    {
        std::string exeDir = getExeDir();
        struct stat st;
        const std::string candidates[] = {
            exeDir + "/web",
            exeDir + "/../share/satellite/web",
            "/usr/local/share/satellite/web",
            "/usr/share/satellite/web",
        };
        for (const auto& c : candidates) {
            if (stat(c.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                g_webDir = c;
                break;
            }
        }
        if (g_webDir.empty()) g_webDir = exeDir + "/web";
    }

    updateService.start();

    std::thread recvTh(receiverThread, std::ref(svc), std::ref(clientAdapter));
    std::thread adminTh(adminHttpThread, std::ref(svc));
    std::thread clientTh(clientApiThread, std::ref(svc));
    std::thread discTh(discoveryThread);
    std::thread mdnsTh(mdnsResponderThread);
    std::thread netlinkTh(netlinkWatcherThread);

    std::fprintf(stderr, "%s running; web UI at http://localhost:%d\n", APP_TITLE,
                 g_config.webPort);

    // Tray-driven GTK loop; falls back to the headless sigwait loop with no
    // display server, on GTK init failure, or when built without SATELLITE_HAS_TRAY.
    bool trayActive = addTrayIcon();

#ifdef SATELLITE_HAS_TRAY
    if (trayActive) {
        // Ignore SIGPIPE (httplib disconnects); bridge SIGINT/SIGTERM to GTK.
        signal(SIGPIPE, SIG_IGN);
        g_unix_signal_add(SIGINT, onTraySignal, nullptr);
        g_unix_signal_add(SIGTERM, onTraySignal, nullptr);
        // Reverse-pairing: a dish request raises a native notification with
        // Accept/Reject so the operator never has to open the web UI.
        setPairRequestListener(notifyPairRequestLinux);
        gtk_main();
        removeTrayIcon();
    }
#endif

    if (!trayActive) {
        sigset_t sigset;
        installHeadlessSignalHandling(sigset);
        int sig = 0;
        while (true) {
            if (sigwait(&sigset, &sig) != 0) break;
            if (sig == SIGPIPE) continue;
            break;
        }
        g_appRunning = false;
        g_httpServer.stop();
        if (g_clientServer) g_clientServer->stop();
    }

    updateService.stop();
    g_updateService = nullptr;

    recvTh.join();
    adminTh.join();
    clientTh.join();
    discTh.join();
    mdnsTh.join();
    netlinkTh.join();

    svc.closeAllSessions();
    saveConfig(g_config);
    netShutdown();
    return 0;
}
