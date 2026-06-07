// SPDX-License-Identifier: LGPL-3.0-or-later
#include "updater_adapter.h"

#include "config.h"
#include "core/github_release.h"
#include "core/version.h"
#include "globals.h"

#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wintrust.h>
#include <softpub.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// Auto-link for the MSVC build (#pragma comment(lib) is a no-op under MinGW
// g++). These libs are also listed in CMakeLists.txt, the authoritative source
// for both toolchains.
#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#endif

namespace {

std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring{};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string fromWide(const std::wstring& w) {
    if (w.empty()) return std::string{};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET p) : h(p) {}
    ~WinHttpHandle() {
        if (h) WinHttpCloseHandle(h);
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& o) noexcept : h(o.h) { o.h = nullptr; }
    operator HINTERNET() const { return h; }
};

struct ParsedUrl {
    std::wstring host;
    INTERNET_PORT port = 443;
    std::wstring path;
    bool https = true;
};

bool parseUrl(const std::wstring& url, ParsedUrl& out) {
    URL_COMPONENTSW u{};
    u.dwStructSize = sizeof(u);
    wchar_t hostBuf[256] = {};
    wchar_t pathBuf[2048] = {};
    u.lpszHostName = hostBuf;
    u.dwHostNameLength = static_cast<DWORD>(std::size(hostBuf));
    u.lpszUrlPath = pathBuf;
    u.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuf));
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &u)) return false;
    out.host.assign(u.lpszHostName, u.dwHostNameLength);
    out.path.assign(u.lpszUrlPath, u.dwUrlPathLength);
    out.port = u.nPort;
    out.https = (u.nScheme == INTERNET_SCHEME_HTTPS);
    if (out.path.empty()) out.path = L"/";
    return true;
}

// HTTPS GET into memory (small responses: releases.json, SHA256SUMS).
bool httpGetToString(const std::wstring& url, std::string& out, std::string& err) {
    ParsedUrl u;
    if (!parseUrl(url, u)) {
        err = "Invalid URL";
        return false;
    }
    // GitHub's per-UA rate-limit bucket keys off this header, so include
    // the running version. SATELLITE_VERSION_STRING is narrow; widen it.
    std::wstring ua = L"Satellite/" + toWide(SATELLITE_VERSION_STRING) + L" (+updater)";
    WinHttpHandle session(WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        err = "WinHttpOpen failed";
        return false;
    }
    DWORD timeoutMs = 15000;
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    WinHttpHandle conn(WinHttpConnect(session, u.host.c_str(), u.port, 0));
    if (!conn) {
        err = "WinHttpConnect failed";
        return false;
    }

    DWORD flags = u.https ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle req(WinHttpOpenRequest(conn, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!req) {
        err = "WinHttpOpenRequest failed";
        return false;
    }

    // Accept GitHub's JSON content type, and identify ourselves so the
    // request hits the per-UA rate-limit bucket rather than the shared
    // anonymous one (still anonymous — no token).
    std::wstring headers = L"Accept: application/vnd.github+json\r\n"
                           L"X-GitHub-Api-Version: 2022-11-28\r\n";

    if (!WinHttpSendRequest(req, headers.c_str(), static_cast<DWORD>(headers.size()),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        err = "WinHttpSendRequest failed";
        return false;
    }
    if (!WinHttpReceiveResponse(req, nullptr)) {
        err = "WinHttpReceiveResponse failed";
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        err = "WinHttpQueryHeaders failed";
        return false;
    }
    if (status < 200 || status >= 300) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP %lu", static_cast<unsigned long>(status));
        err = buf;
        return false;
    }

    out.clear();
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
        size_t before = out.size();
        out.resize(before + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, out.data() + before, avail, &read)) {
            err = "WinHttpReadData failed";
            return false;
        }
        out.resize(before + read);
        if (read == 0) break;
    }
    return true;
}

// HTTPS GET streaming to a file with progress + cancel.
bool httpGetToFile(const std::wstring& url, const std::wstring& dstPath,
                   const std::function<void(uint64_t, uint64_t)>& onProgress,
                   const std::atomic<bool>* cancel, std::string& err) {
    ParsedUrl u;
    if (!parseUrl(url, u)) {
        err = "Invalid URL";
        return false;
    }
    // GitHub's per-UA rate-limit bucket keys off this header, so include
    // the running version. SATELLITE_VERSION_STRING is narrow; widen it.
    std::wstring ua = L"Satellite/" + toWide(SATELLITE_VERSION_STRING) + L" (+updater)";
    WinHttpHandle session(WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        err = "WinHttpOpen failed";
        return false;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);

    WinHttpHandle conn(WinHttpConnect(session, u.host.c_str(), u.port, 0));
    if (!conn) {
        err = "WinHttpConnect failed";
        return false;
    }
    DWORD flags = u.https ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle req(WinHttpOpenRequest(conn, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!req) {
        err = "WinHttpOpenRequest failed";
        return false;
    }
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                            0)) {
        err = "WinHttpSendRequest failed";
        return false;
    }
    if (!WinHttpReceiveResponse(req, nullptr)) {
        err = "WinHttpReceiveResponse failed";
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        err = "WinHttpQueryHeaders failed";
        return false;
    }
    if (status < 200 || status >= 300) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP %lu", static_cast<unsigned long>(status));
        err = buf;
        return false;
    }

    uint64_t total = 0; // Content-Length, for the progress total when present

    {
        wchar_t lenBuf[64] = {};
        DWORD lenSize = sizeof(lenBuf);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                lenBuf, &lenSize, WINHTTP_NO_HEADER_INDEX)) {
            total = static_cast<uint64_t>(_wtoi64(lenBuf));
        }
    }

    // MinGW UCRT's libstdc++ doesn't accept std::wstring overloads on
    // basic_ofstream — go through fromWide() to a narrow path. Our paths
    // under %LOCALAPPDATA%\satellite\updates\ are all ASCII so the
    // narrow conversion is lossless on the active code page.
    std::ofstream out(fromWide(dstPath), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        err = "Cannot open output file";
        return false;
    }

    uint64_t soFar = 0;
    std::vector<char> buf(64 * 1024);
    while (true) {
        if (cancel && cancel->load()) {
            err = "cancelled";
            return false;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            err = "WinHttpQueryDataAvailable failed";
            return false;
        }
        if (avail == 0) break;
        if (avail > buf.size()) buf.resize(avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &read)) {
            err = "WinHttpReadData failed";
            return false;
        }
        if (read == 0) break;
        out.write(buf.data(), read);
        if (!out) {
            err = "Disk write failed";
            return false;
        }
        soFar += read;
        if (onProgress) onProgress(soFar, total);
    }
    out.close();
    if (onProgress) onProgress(soFar, total == 0 ? soFar : total);
    return true;
}

bool sha256OfFile(const std::wstring& path, std::string& hexOut, std::string& err) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (st < 0) {
        err = "BCryptOpenAlgorithmProvider failed";
        return false;
    }
    auto closeAlg = std::unique_ptr<void, void (*)(void*)>(alg, [](void* p) {
        if (p) BCryptCloseAlgorithmProvider(p, 0);
    });

    DWORD hashLen = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen),
                      &cb, 0);
    if (hashLen == 0) {
        err = "SHA-256 length probe failed";
        return false;
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    st = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (st < 0) {
        err = "BCryptCreateHash failed";
        return false;
    }
    auto destroyHash = std::unique_ptr<void, void (*)(void*)>(hash, [](void* p) {
        if (p) BCryptDestroyHash(p);
    });

    std::ifstream f(fromWide(path), std::ios::binary);
    if (!f.is_open()) {
        err = "Cannot open downloaded file for hashing";
        return false;
    }
    std::vector<char> buf(64 * 1024);
    while (f.read(buf.data(), buf.size()) || f.gcount() > 0) {
        std::streamsize n = f.gcount();
        if (n <= 0) break;
        st = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n), 0);
        if (st < 0) {
            err = "BCryptHashData failed";
            return false;
        }
    }

    std::vector<unsigned char> digest(hashLen);
    st = BCryptFinishHash(hash, digest.data(), hashLen, 0);
    if (st < 0) {
        err = "BCryptFinishHash failed";
        return false;
    }
    static const char* kHex = "0123456789abcdef";
    hexOut.clear();
    hexOut.reserve(static_cast<size_t>(hashLen) * 2);
    for (DWORD i = 0; i < hashLen; i++) {
        hexOut += kHex[digest[i] >> 4];
        hexOut += kHex[digest[i] & 0xF];
    }
    return true;
}

// Match SatelliteSetup-*.exe (tolerates the "-unsigned" suffix from CI runs).
bool pickWindowsAsset(const GitHubRelease& rel, GitHubAsset& out) {
    for (const auto& a : rel.assets) {
        if (a.name.size() < std::string("SatelliteSetup-").size() + 4) continue;
        if (a.name.rfind("SatelliteSetup-", 0) != 0) continue;
        if (a.name.size() < 4 || a.name.compare(a.name.size() - 4, 4, ".exe") != 0) continue;
        out = a;
        return true;
    }
    return false;
}

// Digest for `assetName` from the release's SHA256SUMS asset. Empty return is
// non-fatal; the caller decides whether to refuse.
std::string fetchAssetDigest(const GitHubRelease& rel, const std::string& assetName) {
    for (const auto& a : rel.assets) {
        if (a.name == "SHA256SUMS") {
            std::string body, e;
            if (!httpGetToString(toWide(a.browserUrl), body, e)) return "";
            return lookupSha256(body, assetName);
        }
    }
    return "";
}

} // namespace

WindowsUpdaterAdapter::WindowsUpdaterAdapter(std::string owner, std::string repo)
    : owner_(std::move(owner)), repo_(std::move(repo)) {}

bool WindowsUpdaterAdapter::fetchLatestRelease(const std::string& channel,
                                               const std::string& currentVersion, UpdateInfo& out,
                                               std::string& outError) {
    out = {};
    const bool wantPrerelease = (channel == "prerelease");

    // Prerelease channel scans /releases (newest may be a stable one);
    // stable uses /releases/latest.
    std::wstring url;
    if (wantPrerelease) {
        url = L"https://api.github.com/repos/" + toWide(owner_) + L"/" + toWide(repo_) +
              L"/releases?per_page=30";
    } else {
        url = L"https://api.github.com/repos/" + toWide(owner_) + L"/" + toWide(repo_) +
              L"/releases/latest";
    }

    std::string body;
    if (!httpGetToString(url, body, outError)) return false;

    GitHubRelease pick;
    if (wantPrerelease) {
        std::vector<GitHubRelease> list;
        if (!parseGitHubReleaseList(body, list) || list.empty()) {
            outError = "Failed to parse releases list";
            return false;
        }
        // First non-draft entry (API returns newest-first).
        bool found = false;
        for (const auto& r : list) {
            if (r.draft) continue;
            pick = r;
            found = true;
            break;
        }
        if (!found) {
            outError = "No suitable release found";
            return false;
        }
    } else {
        if (!parseGitHubRelease(body, pick)) {
            outError = "Failed to parse release JSON";
            return false;
        }
    }

    if (pick.tagName.empty()) {
        outError = "Release missing tag_name";
        return false;
    }

    GitHubAsset asset;
    if (!pickWindowsAsset(pick, asset)) {
        outError = "No SatelliteSetup-*.exe asset in release " + pick.tagName;
        return false;
    }

    out.version = stripTagPrefix(pick.tagName);
    out.channel = pick.prerelease ? "prerelease" : "stable";
    out.assetName = asset.name;
    out.assetUrl = asset.browserUrl;
    out.assetSize = asset.size;
    out.assetSha256 = fetchAssetDigest(pick, asset.name);
    // Truncate to keep the SSE payload modest; full notes are at htmlUrl.
    out.releaseNotes = pick.body.size() > 8192 ? pick.body.substr(0, 8192) + "..." : pick.body;
    out.htmlUrl = pick.htmlUrl;
    out.publishedAtEpoch = isoToEpoch(pick.publishedAt);
    out.installMethod = InstallMethod::SelfInstall;
    out.available = (out.version != currentVersion); // refined by UpdateService
    return true;
}

bool WindowsUpdaterAdapter::downloadArtifact(
    const UpdateInfo& info, const std::function<void(uint64_t, uint64_t)>& onProgress,
    const std::atomic<bool>* cancel, std::string& outLocalPath, std::string& outError) {
    // Stage under %LOCALAPPDATA% so a Program Files install can download
    // without elevation.
    wchar_t localAppData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        outError = "Cannot resolve %LOCALAPPDATA%";
        return false;
    }
    std::wstring stagingDir = std::wstring(localAppData) + L"\\satellite\\updates";
    SHCreateDirectoryExW(nullptr, stagingDir.c_str(), nullptr);
    std::wstring dst = stagingDir + L"\\" + toWide(info.assetName);

    if (!httpGetToFile(toWide(info.assetUrl), dst, onProgress, cancel, outError)) { return false; }
    outLocalPath = fromWide(dst);
    return true;
}

// Authenticode is checked IN ADDITION to the SHA256SUMS hash: SHA-256 binds
// bytes to the (editable) release page, Authenticode binds to the signing
// identity. Requiring both means an attacker must compromise the signing
// identity AND the release-page content at once.
static bool authenticodeVerify(const std::wstring& path, std::string& err) {
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = path.c_str();

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN | WTD_CACHE_ONLY_URL_RETRIEVAL;

    LONG status = WinVerifyTrust(nullptr, &action, &data);

    // Close out the cached state regardless of outcome.
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);

    if (status == ERROR_SUCCESS) return true;

    char buf[80];
    std::snprintf(buf, sizeof(buf), "Authenticode verification failed (status=0x%08lx)",
                  static_cast<unsigned long>(status));
    err = buf;
    return false;
}

bool WindowsUpdaterAdapter::verifyArtifact(const std::string& localPath, const UpdateInfo& info,
                                           std::string& outError) {
    std::wstring wpath = toWide(localPath);

    // Authenticode FIRST so an altered .exe fails before we hash 12MB.
    // Fail closed on an actively-invalid signature, but a -unsigned asset
    // (TRUST_E_NOSIGNATURE) falls through to SHA-256 so fork builds can update.
    std::string sigErr;
    if (!authenticodeVerify(wpath, sigErr)) {
        bool unsignedRelease = info.assetName.find("-unsigned") != std::string::npos;
        if (!unsignedRelease) {
            outError = sigErr;
            return false;
        }
        // Unsigned release: SHA-256 is now a HARD requirement (nothing else
        // binds the bytes). Fall through.
    }

    if (info.assetSha256.empty()) {
        // No SHA256SUMS asset: a valid signature alone is sufficient; without
        // one, refuse (unsigned-and-unhashed).
        if (sigErr.empty()) return true;
        outError = "No signature and no SHA-256 -- refusing to install: " + sigErr;
        return false;
    }

    std::string actual;
    if (!sha256OfFile(wpath, actual, outError)) return false;

    std::string expected = info.assetSha256;
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](char c) { return static_cast<char>(std::tolower(c)); });
    if (actual != expected) {
        outError = "SHA-256 mismatch: expected " + expected + ", got " + actual;
        return false;
    }
    return true;
}

bool WindowsUpdaterAdapter::applyUpdate(const std::string& localPath, const UpdateInfo& info,
                                        std::string& outError) {
    (void)info;
    // /OTA (installer.iss WantsOTARelaunch) auto-relaunches satellite.exe at the
    // end even silently; /CLOSEAPPLICATIONS lets Inno's Restart Manager close us
    // so the .exe isn't locked during the write.
    SHELLEXECUTEINFOA sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = "open";
    sei.lpFile = localPath.c_str();
    sei.lpParameters = "/VERYSILENT /OTA /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS";
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExA(&sei)) {
        DWORD e = GetLastError();
        char buf[80];
        std::snprintf(buf, sizeof(buf), "ShellExecuteEx failed (GLE=%lu)",
                      static_cast<unsigned long>(e));
        outError = buf;
        return false;
    }
    // Exit now so Inno's Restart Manager replaces the .exe immediately rather
    // than waiting on its close-detection timeout. The webserver keeps serving
    // the "Installing…" SSE briefly until the listener goes down -- intentional.
    PostMessage(g_hwnd, WM_CLOSE, 0, 0);
    return true;
}
