// SPDX-License-Identifier: LGPL-3.0-or-later
// macOS entry point / Composition Root; mirrors platform/windows/main.cpp.
#include "globals.h"
#include "config.h"
#include "crypto.h"
#include "tray.h"
#include "gamepad_adapter.h"
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

#include <sys/stat.h>

#import <AppKit/AppKit.h>

// Cocoa delegate: clean shutdown on terminate.
@interface SatelliteAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation SatelliteAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    g_appRunning = false;
    g_httpServer.stop();
    if (g_clientServer) g_clientServer->stop();
    return NSTerminateNow;
}
@end

int main(int argc, const char* argv[]) {
    (void)argc;
    (void)argv;

    @autoreleasepool {
        // macOS lacks a signed DriverKit equivalent of ViGEmBus, so this build
        // runs the protocol stack but cannot synthesize virtual gamepads.
        fprintf(stderr, "[satellite] macOS stub build: virtual gamepads disabled "
                        "(controller descriptors will apply as backendUnavailable).\n");

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

        GamepadAdapter gamepadAdapter;
        ClientAdapter clientAdapter;
        LogAdapter logAdapter;
        SessionService svc(gamepadAdapter, clientAdapter, logAdapter, deriveSessionKey);

        MacOSUpdaterAdapter updaterAdapter("TinkerNorth", "satellite");
        UpdateService updateService(updaterAdapter, logAdapter, g_config, g_configMtx);
        updateService.setPersistCallback([] {
            std::lock_guard<std::mutex> lk(g_configMtx);
            saveConfig(g_config);
        });
        g_updateService = &updateService;

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

        updateService.start();

        std::thread recvTh(receiverThread, std::ref(svc), std::ref(clientAdapter));
        std::thread adminTh(adminHttpThread, std::ref(svc));
        std::thread clientTh(clientApiThread, std::ref(svc));
        std::thread discTh(discoveryThread);
        std::thread mdnsTh(mdnsResponderThread);

        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        SatelliteAppDelegate* delegate = [[SatelliteAppDelegate alloc] init];
        [app setDelegate:delegate];

        addTrayIcon();

        // Reverse-pairing: a dish request raises a native notification +
        // Accept/Reject alert so the operator never needs the web UI.
        setPairRequestListener(notifyPairRequestMac);

        [app run];
        removeTrayIcon();

        // applicationShouldTerminate: has already flipped the flags.
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
    }
    return 0;
}
