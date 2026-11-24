// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

// The command the Linux updater shows for each install it must not replace
// itself, one per channel. docs/PACKAGING.md's "Update method" table repeats
// them; test_linux_upgrade_commands holds the two together.
namespace satellite::linux_upgrade {

inline constexpr const char* DEB = "sudo apt update && sudo apt install --only-upgrade satellite";
inline constexpr const char* RPM = "sudo dnf upgrade --refresh satellite";
// satellite-bin comes from Satellite's own signed pacman repository, not the
// AUR (whose account registration is closed), so pacman upgrades it.
inline constexpr const char* PACMAN = "sudo pacman -Syu satellite-bin";
inline constexpr const char* SNAP = "sudo snap refresh satellite";
inline constexpr const char* FLATPAK = "flatpak update io.github.tinkernorth.satellite";

} // namespace satellite::linux_upgrade
