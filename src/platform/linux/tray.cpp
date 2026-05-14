// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tray.cpp — libayatana-appindicator status icon and menu (Linux).
 *
 * Menu mirrors the Win32 / macOS trays: Open Web UI / Start-Stop Listener /
 * Quit. The toggle item's label tracks g_listening via a 500 ms GLib timer
 * so we never touch GTK from the worker threads.
 *
 * On vanilla GNOME (no AppIndicator extension) the indicator object is
 * created and "active" on D-Bus, but no shell will render it — there is no
 * portable way to detect that case at runtime. Document it instead.
 */
#include "tray.h"

#ifdef SATELLITE_HAS_TRAY

#include "config.h"
#include "core/update_service.h"

#include <libayatana-appindicator/app-indicator.h>
#include <glib-unix.h>
#include <gtk/gtk.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <string>

static AppIndicator* g_indicator = nullptr;
static GtkWidget* g_toggleItem = nullptr;
static GtkWidget* g_updateItem = nullptr;
static guint g_pollSourceId = 0;
static bool g_lastListening = false;
static UpdateState g_lastUpdateState = UpdateState::Idle;
static std::string g_lastUpdateVersion;

// ── Menu callbacks ──────────────────────────────────────────────────────────
static void onOpenUI(GtkMenuItem*, gpointer) {
    char cmd[96];
    std::snprintf(cmd, sizeof(cmd), "xdg-open http://localhost:%d >/dev/null 2>&1 &",
                  g_config.webPort);
    (void)std::system(cmd);
}

static void onToggleListener(GtkMenuItem*, gpointer) { g_wantListen = !g_listening.load(); }

static void onUpdateClick(GtkMenuItem*, gpointer) {
    if (!g_updateService) return;
    UpdateStatusSnapshot s = g_updateService->snapshot();
    if (s.state == UpdateState::Downloaded) {
        g_updateService->requestInstall();
    } else if (s.state == UpdateState::UpdateAvailable) {
        // Manual installs (Deb/Portable) can't be triggered in-app —
        // open the settings page so the user sees the copy-button.
        if (s.info.installMethod == InstallMethod::SelfInstall) {
            g_updateService->requestDownload();
        }
    } else {
        g_updateService->requestCheck(/*userInitiated=*/true);
    }
    char cmd[112];
    std::snprintf(cmd, sizeof(cmd), "xdg-open http://localhost:%d/settings >/dev/null 2>&1 &",
                  g_config.webPort);
    (void)std::system(cmd);
}

static void onQuit(GtkMenuItem*, gpointer) {
    g_appRunning = false;
    g_wantListen = false;
    g_httpServer.stop();
    if (g_pairSock != INVALID_SOCKET) closesocket(g_pairSock);
    gtk_main_quit();
}

// Keep the toggle + updater items' labels in sync with the actual state.
static gboolean pollListenerState(gpointer) {
    bool listening = g_listening.load();
    if (listening != g_lastListening && g_toggleItem != nullptr) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(g_toggleItem),
                                listening ? "Stop Listener" : "Start Listener");
        g_lastListening = listening;
    }
    if (g_updateService && g_updateItem) {
        UpdateStatusSnapshot s = g_updateService->snapshot();
        if (s.state != g_lastUpdateState || s.info.version != g_lastUpdateVersion) {
            std::string label;
            bool enabled = true;
            if (s.state == UpdateState::Downloaded && s.info.available) {
                label = "Install Update v" + s.info.version;
            } else if (s.state == UpdateState::UpdateAvailable && s.info.available) {
                label = "Download Update v" + s.info.version + "\xE2\x80\xA6";
            } else if (s.state == UpdateState::Downloading || s.state == UpdateState::Verifying) {
                label = "Downloading update\xE2\x80\xA6";
                enabled = false;
            } else if (s.state == UpdateState::Checking) {
                label = "Checking for updates\xE2\x80\xA6";
                enabled = false;
            } else {
                label = "Check for Updates\xE2\x80\xA6";
            }
            gtk_menu_item_set_label(GTK_MENU_ITEM(g_updateItem), label.c_str());
            gtk_widget_set_sensitive(g_updateItem, enabled);
            g_lastUpdateState = s.state;
            g_lastUpdateVersion = s.info.version;
        }
    }
    return G_SOURCE_CONTINUE;
}

// Resolve the icon: prefer the bundled web/icon.png (dev / portable layout),
// otherwise fall back to the freedesktop "input-gaming" themed name which
// virtually every icon theme ships.
static void applyIcon(AppIndicator* ind) {
    if (!g_webDir.empty()) {
        struct stat st;
        std::string iconFile = g_webDir + "/icon.png";
        if (stat(iconFile.c_str(), &st) == 0) {
            // app-indicator looks up names within the theme path: place the
            // file as <dir>/icon.png and ask for "icon" (no extension).
            app_indicator_set_icon_theme_path(ind, g_webDir.c_str());
            app_indicator_set_icon_full(ind, "icon", APP_TITLE);
            return;
        }
    }
    app_indicator_set_icon_full(ind, "input-gaming", APP_TITLE);
}

// ── Public API ──────────────────────────────────────────────────────────────
bool addTrayIcon() {
    if (getenv("DISPLAY") == nullptr && getenv("WAYLAND_DISPLAY") == nullptr) { return false; }
    int argc = 0;
    char** argv = nullptr;
    if (!gtk_init_check(&argc, &argv)) return false;

    g_indicator =
        app_indicator_new("satellite", "input-gaming", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    if (g_indicator == nullptr) return false;
    app_indicator_set_status(g_indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(g_indicator, APP_TITLE);
    applyIcon(g_indicator);

    GtkWidget* menu = gtk_menu_new();

    GtkWidget* openItem = gtk_menu_item_new_with_label("Open Web UI");
    g_signal_connect(openItem, "activate", G_CALLBACK(onOpenUI), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), openItem);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    g_lastListening = g_listening.load();
    g_toggleItem =
        gtk_menu_item_new_with_label(g_lastListening ? "Stop Listener" : "Start Listener");
    g_signal_connect(g_toggleItem, "activate", G_CALLBACK(onToggleListener), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_toggleItem);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    // Updater item — label kept current by pollListenerState().
    g_updateItem = gtk_menu_item_new_with_label("Check for Updates\xE2\x80\xA6");
    g_signal_connect(g_updateItem, "activate", G_CALLBACK(onUpdateClick), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_updateItem);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget* quitItem = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quitItem, "activate", G_CALLBACK(onQuit), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quitItem);

    gtk_widget_show_all(menu);
    app_indicator_set_menu(g_indicator, GTK_MENU(menu));

    g_pollSourceId = g_timeout_add(500, pollListenerState, nullptr);
    return true;
}

void removeTrayIcon() {
    if (g_pollSourceId != 0) {
        g_source_remove(g_pollSourceId);
        g_pollSourceId = 0;
    }
    if (g_indicator != nullptr) {
        app_indicator_set_status(g_indicator, APP_INDICATOR_STATUS_PASSIVE);
        g_object_unref(G_OBJECT(g_indicator));
        g_indicator = nullptr;
    }
    g_toggleItem = nullptr;
    g_updateItem = nullptr;
}

#else // SATELLITE_HAS_TRAY

bool addTrayIcon() { return false; }
void removeTrayIcon() {}

#endif // SATELLITE_HAS_TRAY
