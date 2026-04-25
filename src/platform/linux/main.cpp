/*
 * main.cpp — Linux entry point / Composition Root.
 *
 * Mirrors platform/windows/main.cpp and platform/macos/main.mm:
 *   - Initialize networking + libsodium
 *   - Load config, apply autoStart
 *   - Wire adapters into SessionService
 *   - Spawn the four worker threads (receiver, http, pairing, discovery)
 *   - Run the GTK main loop so the libayatana-appindicator status icon is
 *     responsive; fall back to a sigwait-based headless loop on truly
 *     headless boxes or when the binary was built without tray support.
 */
#include "globals.h"
#include "config.h"
#include "crypto.h"
#include "gamepad_adapter.h"
#include "tray.h"

#include "net/receiver.h"
#include "net/webserver.h"
#include "net/pairing.h"
#include "net/discovery.h"

#include "adapters/client_adapter.h"
#include "adapters/log_adapter.h"

#include "core/session_service.h"

#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>

#include <cstdio>

#ifdef SATELLITE_HAS_TRAY
#include <glib-unix.h>
#include <gtk/gtk.h>
#endif

// Headless main-loop signal blocking: SIGINT/SIGTERM are delivered via sigwait
// in the main thread, avoiding default-termination during long-running syscalls
// in worker threads (recvfrom, accept, ...). SIGPIPE is always ignored — it
// fires when an httplib client disconnects mid-write.
static void installHeadlessSignalHandling(sigset_t& set) {
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

#ifdef SATELLITE_HAS_TRAY
// Bridge SIGINT/SIGTERM into the GTK main loop. The handler runs on whatever
// thread the kernel chose; g_unix_signal_add internally pipes the signal so
// the actual callback fires on the main loop.
static gboolean onTraySignal(gpointer) {
    g_appRunning = false;
    g_wantListen = false;
    g_httpServer.stop();
    if (g_pairSock != INVALID_SOCKET) closesocket(g_pairSock);
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

    if (g_config.autoStart) g_wantListen = true;

    // ── Composition Root ────────────────────────────────────────
    GamepadAdapter gamepadAdapter;
    ClientAdapter clientAdapter;
    LogAdapter logAdapter;
    SessionService svc(gamepadAdapter, clientAdapter, logAdapter);

    // Resolve web/ directory:
    //   1. <exeDir>/web            — dev / side-by-side layout
    //   2. <exeDir>/../share/satellite/web — FHS install from prefix
    //   3. /usr/local/share/satellite/web  — manual sudo install
    //   4. /usr/share/satellite/web        — package install
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

    std::thread recvTh(receiverThread, std::ref(svc), std::ref(clientAdapter));
    std::thread httpTh(httpThread, std::ref(svc));
    std::thread pairTh(pairingThread);
    std::thread discTh(discoveryThread);

    std::fprintf(stderr, "%s running — web UI at http://localhost:%d\n", APP_TITLE,
                 g_config.webPort);

    // Try the tray-driven GTK loop first. Falls back to a sigwait-based
    // headless loop if there's no display server, the AppIndicator/GTK init
    // fails, or the binary was built without SATELLITE_HAS_TRAY.
    bool trayActive = addTrayIcon();

#ifdef SATELLITE_HAS_TRAY
    if (trayActive) {
        // Ignore SIGPIPE (httplib disconnects); bridge SIGINT/SIGTERM to GTK.
        signal(SIGPIPE, SIG_IGN);
        g_unix_signal_add(SIGINT, onTraySignal, nullptr);
        g_unix_signal_add(SIGTERM, onTraySignal, nullptr);
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
        g_wantListen = false;
        g_httpServer.stop();
        if (g_pairSock != INVALID_SOCKET) closesocket(g_pairSock);
    }

    recvTh.join();
    httpTh.join();
    pairTh.join();
    discTh.join();

    svc.closeAllSessions();
    saveConfig(g_config);
    netShutdown();
    return 0;
}
