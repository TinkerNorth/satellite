// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tray.cpp — libayatana-appindicator status icon and menu (Linux).
 *
 * Menu mirrors the Win32 / macOS trays: Open Web UI / Check for Updates /
 * Quit.
 *
 * On vanilla GNOME (no AppIndicator extension) the indicator object is
 * created and "active" on D-Bus, but no shell will render it — there is no
 * portable way to detect that case at runtime. Document it instead.
 */
#include "tray.h"

#ifdef SATELLITE_HAS_TRAY

#include "config.h"
#include "core/update_service.h"
#include "net/pairing.h"
#include "net/pairing_service.h"

#include <libayatana-appindicator/app-indicator.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#ifdef SATELLITE_HAS_LIBNOTIFY
#include <libnotify/notify.h>
#endif

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

static AppIndicator* g_indicator = nullptr;
static GtkWidget* g_updateItem = nullptr;
static guint g_pollSourceId = 0;
static UpdateState g_lastUpdateState = UpdateState::Idle;
static std::string g_lastUpdateVersion;

// ── Menu callbacks ──────────────────────────────────────────────────────────
static void onOpenUI(GtkMenuItem*, gpointer) {
    char cmd[96];
    std::snprintf(cmd, sizeof(cmd), "xdg-open http://localhost:%d >/dev/null 2>&1 &",
                  g_config.webPort);
    (void)std::system(cmd);
}

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
    g_httpServer.stop();
    if (g_pairSock != INVALID_SOCKET) closesocket(g_pairSock);
    gtk_main_quit();
}

// Keep the updater item's label in sync with the actual state.
static gboolean pollMenuState(gpointer) {
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

// ── Reverse-pairing native prompt (libnotify) ───────────────────────────────
#ifdef SATELLITE_HAS_LIBNOTIFY
static void onPairAccept(NotifyNotification*, char* action, gpointer user_data) {
    (void)action;
    const char* id = static_cast<const char*>(user_data);
    if (id != nullptr) confirmPairing(id);
}

static void onPairReject(NotifyNotification*, char* action, gpointer user_data) {
    (void)action;
    const char* id = static_cast<const char*>(user_data);
    if (id != nullptr) declinePairing(id);
}

// Release our GObject ref once the notification closes (after any action fires).
static void onPairClosed(NotifyNotification* n, gpointer) { g_object_unref(n); }

// Runs on the GTK main loop (g_idle_add target). Shows the dish's PIN so the
// operator can confirm it matches the device — that visual match is the auth.
static gboolean showPairPromptIdle(gpointer data) {
    std::unique_ptr<std::string> deviceId(static_cast<std::string*>(data));
    std::string name, ip, pin;
    int secs = 0;
    if (!pairRequestSnapshot(*deviceId, name, ip, pin, secs)) return G_SOURCE_REMOVE;

    const std::string body = (name.empty() ? std::string("A device") : name) + " (" + ip +
                             ") wants to pair.\nPIN on the device: " + pin +
                             "\nConfirm it matches the device, then Accept.";
    NotifyNotification* n =
        notify_notification_new("Pairing request", body.c_str(), "input-gaming");
    notify_notification_set_timeout(n, NOTIFY_EXPIRES_NEVER);
    // The action user_data must outlive the notification → g_strdup + g_free.
    notify_notification_add_action(n, "accept", "Accept", onPairAccept, g_strdup(deviceId->c_str()),
                                   g_free);
    notify_notification_add_action(n, "reject", "Reject", onPairReject, g_strdup(deviceId->c_str()),
                                   g_free);
    g_signal_connect(n, "closed", G_CALLBACK(onPairClosed), nullptr);
    notify_notification_show(n, nullptr);
    return G_SOURCE_REMOVE;
}

void notifyPairRequestLinux(const std::string& deviceId) {
    // Marshal onto the GTK main loop — libnotify/GLib isn't thread-safe off it,
    // and the listener fires on the HTTP thread.
    g_idle_add(showPairPromptIdle, new std::string(deviceId));
}
#else
void notifyPairRequestLinux(const std::string&) {}
#endif

// ── Public API ──────────────────────────────────────────────────────────────
bool addTrayIcon() {
    if (getenv("DISPLAY") == nullptr && getenv("WAYLAND_DISPLAY") == nullptr) { return false; }
    int argc = 0;
    char** argv = nullptr;
    if (!gtk_init_check(&argc, &argv)) return false;
#ifdef SATELLITE_HAS_LIBNOTIFY
    notify_init(APP_TITLE);
#endif

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

    // Updater item — label kept current by pollMenuState().
    g_updateItem = gtk_menu_item_new_with_label("Check for Updates\xE2\x80\xA6");
    g_signal_connect(g_updateItem, "activate", G_CALLBACK(onUpdateClick), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_updateItem);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget* quitItem = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quitItem, "activate", G_CALLBACK(onQuit), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quitItem);

    gtk_widget_show_all(menu);
    app_indicator_set_menu(g_indicator, GTK_MENU(menu));

    g_pollSourceId = g_timeout_add(500, pollMenuState, nullptr);
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
    g_updateItem = nullptr;
#ifdef SATELLITE_HAS_LIBNOTIFY
    notify_uninit();
#endif
}

#else // SATELLITE_HAS_TRAY

bool addTrayIcon() { return false; }
void removeTrayIcon() {}
void notifyPairRequestLinux(const std::string&) {}

#endif // SATELLITE_HAS_TRAY
