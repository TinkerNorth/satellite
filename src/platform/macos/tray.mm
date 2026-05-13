// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tray.mm — macOS menu-bar status item and menu actions.
 */
#include "tray.h"
#include "config.h"
#include "core/update_service.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

// ── Menu action target ─────────────────────────────────────────────────────
@interface SatelliteTrayTarget : NSObject
- (void)openUI:(id)sender;
- (void)toggleListener:(id)sender;
- (void)updateAction:(id)sender;
- (void)quit:(id)sender;
- (void)rebuildMenu;
@end

static NSStatusItem* g_statusItem = nil;
static SatelliteTrayTarget* g_target = nil;

@implementation SatelliteTrayTarget

- (void)openUI:(id)sender {
    (void)sender;
    NSString* url = [NSString stringWithFormat:@"http://localhost:%d", g_config.webPort];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
}

- (void)toggleListener:(id)sender {
    (void)sender;
    g_wantListen = !g_listening.load();
    [self rebuildMenu];
}

- (void)updateAction:(id)sender {
    (void)sender;
    if (!g_updateService) return;
    UpdateStatusSnapshot s = g_updateService->snapshot();
    if (s.state == UpdateState::Downloaded) {
        g_updateService->requestInstall();
    } else if (s.state == UpdateState::UpdateAvailable &&
               s.info.installMethod == InstallMethod::SelfInstall) {
        g_updateService->requestDownload();
    } else {
        g_updateService->requestCheck(/*userInitiated=*/true);
    }
    NSString* url = [NSString stringWithFormat:@"http://localhost:%d/settings", g_config.webPort];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
    [self rebuildMenu];
}

- (void)quit:(id)sender {
    (void)sender;
    [NSApp terminate:nil];
}

- (void)rebuildMenu {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];

    NSMenuItem* openItem = [[NSMenuItem alloc] initWithTitle:@"Open Web UI"
                                                      action:@selector(openUI:)
                                               keyEquivalent:@""];
    [openItem setTarget:self];
    [menu addItem:openItem];

    [menu addItem:[NSMenuItem separatorItem]];

    NSString* toggleTitle = g_listening.load() ? @"Stop Listener" : @"Start Listener";
    NSMenuItem* toggleItem = [[NSMenuItem alloc] initWithTitle:toggleTitle
                                                        action:@selector(toggleListener:)
                                                 keyEquivalent:@""];
    [toggleItem setTarget:self];
    [menu addItem:toggleItem];

    [menu addItem:[NSMenuItem separatorItem]];

    // Updater — label tracks state. Disabled while in flight.
    NSString* updateTitle = @"Check for Updates…";
    BOOL updateEnabled = (g_updateService != nullptr);
    if (g_updateService) {
        UpdateStatusSnapshot s = g_updateService->snapshot();
        if (s.state == UpdateState::Downloaded && s.info.available) {
            updateTitle = [NSString stringWithFormat:@"Install Update v%s", s.info.version.c_str()];
        } else if (s.state == UpdateState::UpdateAvailable && s.info.available) {
            updateTitle = [NSString stringWithFormat:@"Download Update v%s…",
                                                     s.info.version.c_str()];
        } else if (s.state == UpdateState::Downloading || s.state == UpdateState::Verifying) {
            updateTitle = @"Downloading update…";
            updateEnabled = NO;
        } else if (s.state == UpdateState::Checking) {
            updateTitle = @"Checking for updates…";
            updateEnabled = NO;
        }
    }
    NSMenuItem* updateItem = [[NSMenuItem alloc] initWithTitle:updateTitle
                                                        action:@selector(updateAction:)
                                                 keyEquivalent:@""];
    [updateItem setTarget:self];
    [updateItem setEnabled:updateEnabled];
    [menu addItem:updateItem];

    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit"
                                                      action:@selector(quit:)
                                               keyEquivalent:@"q"];
    [quitItem setTarget:self];
    [menu addItem:quitItem];

    [g_statusItem setMenu:menu];
}

@end

// ── Public API ─────────────────────────────────────────────────────────────
void addTrayIcon() {
    g_statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    NSStatusBarButton* btn = [g_statusItem button];
    [btn setTitle:@"🛰"];
    [btn setToolTip:@(APP_TITLE)];

    g_target = [[SatelliteTrayTarget alloc] init];
    [g_target rebuildMenu];
}

void removeTrayIcon() {
    if (g_statusItem != nil) {
        [[NSStatusBar systemStatusBar] removeStatusItem:g_statusItem];
        g_statusItem = nil;
    }
    g_target = nil;
}
