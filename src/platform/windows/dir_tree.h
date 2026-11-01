// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <string>

namespace lifecycle {

inline bool ensureDirectoryTree(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t cut = path.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 0 && !ensureDirectoryTree(path.substr(0, cut))) {
        return false;
    }
    return CreateDirectoryW(path.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

} // namespace lifecycle
