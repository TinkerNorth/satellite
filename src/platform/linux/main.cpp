// SPDX-License-Identifier: LGPL-3.0-or-later
// Linux entry point / composition root. Mirrors the Windows and macOS mains.
#include "globals.h"
#include "config.h"
#include "crypto.h"
#include "gamepad_adapter.h"
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

#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>

#include <cstdio>

#ifdef SATELLITE_HAS_TRAY
#include <glib-unix.h>
#include <gtk/gtk.h>
#endif

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

    svc.closeAllSessions();
    saveConfig(g_config);
    netShutdown();
    return 0;
}
