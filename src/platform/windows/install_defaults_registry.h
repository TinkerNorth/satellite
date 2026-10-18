// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <string>

#include "core/install_defaults.h"

namespace installdefaults {

inline const wchar_t* const kRegistrySubkey = L"SOFTWARE\\TinkerNorth\\Satellite\\InstallDefaults";

inline void readSwitch(HKEY key, const wchar_t* name, bool& out) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) ==
        ERROR_SUCCESS) {
        out = value != 0;
    }
}

inline satellite::InstallDefaults readInstallDefaults(HKEY root, const wchar_t* subkey) {
    satellite::InstallDefaults d;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return d;
    wchar_t stamp[128] = {};
    DWORD size = sizeof(stamp);
    if (RegGetValueW(key, nullptr, L"Stamp", RRF_RT_REG_SZ, nullptr, stamp, &size) ==
        ERROR_SUCCESS) {
        char narrow[256] = {};
        if (WideCharToMultiByte(CP_UTF8, 0, stamp, -1, narrow, sizeof(narrow), nullptr, nullptr) >
            0) {
            d.stamp.assign(narrow);
        }
    }
    readSwitch(key, L"AutoCheckUpdates", d.autoCheck);
    readSwitch(key, L"CrashReports", d.crashReporting);
    RegCloseKey(key);
    return d;
}

inline satellite::InstallDefaults readInstallDefaults() {
    return readInstallDefaults(HKEY_LOCAL_MACHINE, kRegistrySubkey);
}

} // namespace installdefaults
