/*
 * main.mm — macOS entry point / Composition Root.
 *
 * Mirrors platform/windows/main.cpp:
 *   - Initialize networking + libsodium
 *   - Load config, apply autoStart
 *   - Wire adapters into SessionService
 *   - Spawn the four worker threads (receiver, http, pairing, discovery)
 *   - Run the Cocoa event loop so the menu-bar status item is responsive
 */
#include "globals.h"
#include "config.h"
#include "crypto.h"
#include "tray.h"
#include "gamepad_adapter.h"

#include "net/receiver.h"
#include "net/webserver.h"
#include "net/pairing.h"
#include "net/discovery.h"

#include "adapters/client_adapter.h"
#include "adapters/log_adapter.h"

#include "core/session_service.h"

#include <sys/stat.h>

#import <AppKit/AppKit.h>

// ── Cocoa delegate: clean shutdown on terminate ─────────────────────────────
@interface SatelliteAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation SatelliteAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    g_appRunning = false;
    g_wantListen = false;
    g_httpServer.stop();
    if (g_pairSock != INVALID_SOCKET) closesocket(g_pairSock);
    return NSTerminateNow;
}
@end

int main(int argc, const char* argv[]) {
    (void)argc;
    (void)argv;

    @autoreleasepool {
        if (!netInit()) {
            fprintf(stderr, "Failed to initialize network subsystem\n");
            return 1;
        }

        if (!sodiumInit()) {
            fprintf(stderr, "Failed to initialize libsodium\n");
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

        // Inside an .app bundle the binary lives at Contents/MacOS/; the web
        // UI is staged into Contents/Resources/web by CMake. Fall back to a
        // sibling web/ directory for non-bundle / dev builds.
        {
            std::string exeDir = getExeDir();
            std::string bundled = exeDir + "/../Resources/web";
            struct stat st;
            if (stat(bundled.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                g_webDir = bundled;
            } else {
                g_webDir = exeDir + "/web";
            }
        }

        std::thread recvTh(receiverThread, std::ref(svc), std::ref(clientAdapter));
        std::thread httpTh(httpThread, std::ref(svc));
        std::thread pairTh(pairingThread);
        std::thread discTh(discoveryThread);

        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        SatelliteAppDelegate* delegate = [[SatelliteAppDelegate alloc] init];
        [app setDelegate:delegate];

        addTrayIcon();
        [app run];
        removeTrayIcon();

        // Cleanup — applicationShouldTerminate: has already flipped the flags.
        recvTh.join();
        httpTh.join();
        pairTh.join();
        discTh.join();

        svc.closeAllSessions();
        saveConfig(g_config);
        netShutdown();
    }
    return 0;
}
