// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <charconv>
#include <string>

namespace uiaccess {

inline bool tokenHasUiAccess() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD uiAccess = 0;
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenUIAccess, &uiAccess, sizeof(uiAccess), &size);
    CloseHandle(token);
    return ok && uiAccess != 0;
}

inline DWORD tokenIntegrityRid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 0;
    struct {
        TOKEN_MANDATORY_LABEL label;
        BYTE sid[SECURITY_MAX_SID_SIZE];
    } info{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenIntegrityLevel, &info, sizeof(info), &size);
    CloseHandle(token);
    const PSID sid = info.label.Label.Sid;
    if (!ok || sid == nullptr || !IsValidSid(sid)) return 0;
    const DWORD count = *GetSidSubAuthorityCount(sid);
    if (count == 0) return 0;
    return *GetSidSubAuthority(sid, count - 1);
}

inline std::string integrityLevelName(DWORD rid) {
    switch (rid) {
    case SECURITY_MANDATORY_LOW_RID:
        return "low";
    case SECURITY_MANDATORY_MEDIUM_RID:
        return "medium";
    case SECURITY_MANDATORY_MEDIUM_RID + 0x10:
        return "medium with UIAccess";
    case SECURITY_MANDATORY_MEDIUM_PLUS_RID:
        return "medium plus";
    case SECURITY_MANDATORY_HIGH_RID:
        return "high";
    case SECURITY_MANDATORY_SYSTEM_RID:
        return "system";
    default:
        break;
    }
    char digits[8] = {};
    char* const end = std::to_chars(digits, digits + sizeof(digits), rid, 16).ptr;
    return "0x" + std::string(digits, end);
}

inline std::string describePointerInjection(bool uiAccess, DWORD rid) {
    const std::string level = integrityLevelName(rid);
    if (uiAccess && rid >= SECURITY_MANDATORY_HIGH_RID) {
        return "Pointer injection has UIAccess (integrity level: " + level +
               "): injected pointer input reaches a foreground window of up to high integrity";
    }
    if (uiAccess) {
        return "Pointer injection has UIAccess (integrity level: " + level +
               "): injected pointer input reaches foreground windows up to its own integrity "
               "level; a standard user's UIAccess process does not reach elevated windows";
    }
    return "Pointer injection runs without UIAccess (integrity level: " + level +
           "): Windows drops injected pointer input while the foreground window belongs to a "
           "more privileged process; a signed build installed under Program Files lifts this";
}

} // namespace uiaccess
