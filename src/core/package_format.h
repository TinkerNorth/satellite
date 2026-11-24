// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

// The native package a Linux build was made for, as CMake's
// SATELLITE_PACKAGE_FORMAT names it. The updater reads it instead of a
// package manager's database to tell a .deb install from an .rpm one.
namespace satellite {

enum class PackageFormat : uint8_t { None, Deb, Rpm };

constexpr PackageFormat parsePackageFormat(std::string_view name) {
    if (name == "deb") return PackageFormat::Deb;
    if (name == "rpm") return PackageFormat::Rpm;
    return PackageFormat::None;
}

} // namespace satellite
