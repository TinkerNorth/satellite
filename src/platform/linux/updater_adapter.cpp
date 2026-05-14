// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

#include "updater_adapter.h"

#include "config.h"
#include "core/github_release.h"
#include "core/version.h"

#include <curl/curl.h>
#include <sodium.h>   // crypto_hash_sha256 — already linked

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

// ── libcurl wrappers ─────────────────────────────────────────────────────
// We dlopen libcurl at runtime via the system linker so distros without
// libcurl-dev installed at build-time can still build (we only need libcurl
// at runtime for the updater feature). For simplicity though we link it
// directly — every desktop distro ships libcurl as a base package.

struct CurlWriteCtx {
    std::string* str = nullptr;
    std::ofstream* file = nullptr;
    uint64_t soFar = 0;
};

size_t writeStringCb(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* ctx = static_cast<CurlWriteCtx*>(ud);
    size_t n = size * nmemb;
    if (ctx->str) ctx->str->append(ptr, n);
    ctx->soFar += n;
    return n;
}

size_t writeFileCb(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* ctx = static_cast<CurlWriteCtx*>(ud);
    size_t n = size * nmemb;
    if (ctx->file) ctx->file->write(ptr, n);
    ctx->soFar += n;
    return n;
}

struct ProgressCtx {
    std::function<void(uint64_t, uint64_t)> cb;
    const std::atomic<bool>* cancel = nullptr;
};

int xferInfoCb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<ProgressCtx*>(clientp);
    if (ctx->cancel && ctx->cancel->load()) return 1; // abort
    if (ctx->cb) ctx->cb(static_cast<uint64_t>(dlnow), static_cast<uint64_t>(dltotal));
    return 0;
}

bool httpGetToString(const std::string& url, std::string& out, std::string& err) {
    CURL* c = curl_easy_init();
    if (!c) {
        err = "curl_easy_init failed";
        return false;
    }
    std::string ua = std::string("Satellite/") + SATELLITE_VERSION_STRING + " (+updater)";
    CurlWriteCtx ctx;
    ctx.str = &out;
    out.clear();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeStringCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
    // TLS verification ON (the default, but be explicit).
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        err = curl_easy_strerror(rc);
        return false;
    }
    if (status < 200 || status >= 300) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP %ld", status);
        err = buf;
        return false;
    }
    return true;
}

bool httpGetToFile(const std::string& url, const std::string& dstPath,
                   const std::function<void(uint64_t, uint64_t)>& onProgress,
                   const std::atomic<bool>* cancel, std::string& err) {
    CURL* c = curl_easy_init();
    if (!c) {
        err = "curl_easy_init failed";
        return false;
    }
    std::ofstream out(dstPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        err = "Cannot open output file";
        curl_easy_cleanup(c);
        return false;
    }

    std::string ua = std::string("Satellite/") + SATELLITE_VERSION_STRING + " (+updater)";
    CurlWriteCtx wctx;
    wctx.file = &out;
    ProgressCtx pctx;
    pctx.cb = onProgress;
    pctx.cancel = cancel;

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 0L);            // no overall timeout (large file)
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeFileCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &wctx);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferInfoCb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &pctx);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    out.close();

    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        std::remove(dstPath.c_str());
        err = "cancelled";
        return false;
    }
    if (rc != CURLE_OK) {
        std::remove(dstPath.c_str());
        err = curl_easy_strerror(rc);
        return false;
    }
    if (status < 200 || status >= 300) {
        std::remove(dstPath.c_str());
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP %ld", status);
        err = buf;
        return false;
    }
    return true;
}

// ── Asset selection ──────────────────────────────────────────────────────
bool pickAppImageAsset(const GitHubRelease& rel, GitHubAsset& out) {
    for (const auto& a : rel.assets) {
        if (a.name.find("-x86_64.AppImage") != std::string::npos &&
            a.name.rfind("satellite-", 0) == 0) {
            out = a;
            return true;
        }
    }
    return false;
}

bool pickDebAsset(const GitHubRelease& rel, GitHubAsset& out) {
    for (const auto& a : rel.assets) {
        if (a.name.size() >= 4 && a.name.compare(a.name.size() - 4, 4, ".deb") == 0 &&
            a.name.rfind("satellite_", 0) == 0) {
            out = a;
            return true;
        }
    }
    return false;
}

bool pickRpmAsset(const GitHubRelease& rel, GitHubAsset& out) {
    for (const auto& a : rel.assets) {
        if (a.name.size() >= 4 && a.name.compare(a.name.size() - 4, 4, ".rpm") == 0 &&
            a.name.rfind("satellite-", 0) == 0) {
            out = a;
            return true;
        }
    }
    return false;
}

std::string fetchAssetDigest(const GitHubRelease& rel, const std::string& assetName) {
    for (const auto& a : rel.assets) {
        if (a.name == "SHA256SUMS") {
            std::string body, e;
            if (!httpGetToString(a.browserUrl, body, e)) return "";
            return lookupSha256(body, assetName);
        }
    }
    return "";
}

// ── SHA-256 via libsodium ────────────────────────────────────────────────
bool sha256OfFile(const std::string& path, std::string& hexOut, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "Cannot open file for hashing";
        return false;
    }
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);
    unsigned char buf[64 * 1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        crypto_hash_sha256_update(&st, buf, n);
    }
    std::fclose(f);
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256_final(&st, digest);
    static const char* kHex = "0123456789abcdef";
    hexOut.clear();
    hexOut.reserve(crypto_hash_sha256_BYTES * 2);
    for (size_t i = 0; i < sizeof(digest); i++) {
        hexOut += kHex[digest[i] >> 4];
        hexOut += kHex[digest[i] & 0xF];
    }
    return true;
}

} // namespace

// ── LinuxUpdaterAdapter ──────────────────────────────────────────────────
LinuxUpdaterAdapter::InstallType LinuxUpdaterAdapter::detectInstallType() const {
    // 1. AppImage: $APPIMAGE env var is set by the AppImage runtime.
    //    Most reliable — the runtime guarantees it.
    if (const char* env = std::getenv("APPIMAGE"); env != nullptr && env[0] != '\0') {
        struct stat st;
        if (stat(env, &st) == 0) return InstallType::AppImage;
    }

    // 2. Path-based detection for everything that lives under a system
    //    bin/. We avoid spawning dpkg/rpm/pacman (slow, not always
    //    available) and key off filesystem artefacts:
    //
    //      /opt/satellite/satellite.AppImage     → AUR satellite-bin
    //        (the shim at /usr/bin/satellite execs it)
    //      /usr/bin/satellite + /var/lib/dpkg    → Deb
    //      /usr/bin/satellite + /var/lib/rpm     → Rpm
    //      /usr/local/bin/satellite              → Deb (manual `dpkg -i`)
    //                                              or generic local install
    char buf[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string p(buf);
        struct stat st;

        // AUR satellite-bin keeps the real binary in /opt/satellite/ and
        // the shim in /usr/bin. Either path resolves here.
        if (p == "/opt/satellite/satellite.AppImage" ||
            stat("/opt/satellite/satellite.AppImage", &st) == 0) {
            return InstallType::Aur;
        }

        if (p == "/usr/bin/satellite" || p == "/usr/local/bin/satellite") {
            // Discriminate dpkg vs rpm by which package database exists.
            // Both can coexist (e.g. Fedora with alien-installed .debs)
            // but a satellite binary in /usr/bin will only have been put
            // there by one of them.
            const bool hasDpkg = (stat("/var/lib/dpkg/status", &st) == 0);
            const bool hasRpm = (stat("/var/lib/rpm", &st) == 0);
            if (hasRpm && !hasDpkg) return InstallType::Rpm;
            if (hasDpkg && !hasRpm) return InstallType::Deb;
            // Tie-break: prefer the family whose package actually owns
            // the file. A query to dpkg's own info-list is cheap.
            if (hasDpkg) {
                std::string dpkgInfo = "/var/lib/dpkg/info/satellite.list";
                if (stat(dpkgInfo.c_str(), &st) == 0) return InstallType::Deb;
            }
            if (hasRpm) return InstallType::Rpm;
            return InstallType::Deb;
        }
    }
    return InstallType::Portable;
}

LinuxUpdaterAdapter::LinuxUpdaterAdapter(std::string owner, std::string repo)
    : owner_(std::move(owner)), repo_(std::move(repo)) {
    installType_ = detectInstallType();
    switch (installType_) {
    case InstallType::AppImage: platformId_ = "linux-appimage"; break;
    case InstallType::Deb:      platformId_ = "linux-deb"; break;
    case InstallType::Rpm:      platformId_ = "linux-rpm"; break;
    case InstallType::Aur:      platformId_ = "linux-aur"; break;
    case InstallType::Portable: platformId_ = "linux-portable"; break;
    }
    // Initialize libcurl once. Safe to call multiple times — the second
    // call is a no-op when already initialized.
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

bool LinuxUpdaterAdapter::fetchLatestRelease(const std::string& channel,
                                              const std::string& currentVersion,
                                              UpdateInfo& out, std::string& outError) {
    out = {};
    const bool wantPrerelease = (channel == "prerelease");
    std::string url = "https://api.github.com/repos/" + owner_ + "/" + repo_;
    if (wantPrerelease) {
        url += "/releases?per_page=30";
    } else {
        url += "/releases/latest";
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

    // Asset and install method depend on how we were installed.
    GitHubAsset asset;
    std::string manual;
    InstallMethod method = InstallMethod::SelfInstall;
    switch (installType_) {
    case InstallType::AppImage:
        if (!pickAppImageAsset(pick, asset)) {
            outError = "No AppImage asset in release " + pick.tagName;
            return false;
        }
        method = InstallMethod::SelfInstall;
        break;
    case InstallType::Deb:
        if (!pickDebAsset(pick, asset)) {
            outError = "No .deb asset in release " + pick.tagName;
            return false;
        }
        method = InstallMethod::Manual;
        // Users on our APT repo get this for free from `apt upgrade`; the
        // instruction is also correct for users who installed via a local
        // `dpkg -i ./satellite_*.deb`, since apt will offer the matching
        // upgrade once they've added the repo from the install instructions
        // at https://tinkernorth.github.io/satellite/.
        manual = "sudo apt update && sudo apt install --only-upgrade satellite";
        break;
    case InstallType::Rpm:
        if (!pickRpmAsset(pick, asset)) {
            outError = "No .rpm asset in release " + pick.tagName;
            return false;
        }
        method = InstallMethod::Manual;
        manual = "sudo dnf upgrade --refresh satellite";
        break;
    case InstallType::Aur:
        // The AUR -bin package wraps the AppImage, so the artifact metadata
        // we surface is the AppImage (used purely for "show release notes"
        // — we don't try to download it ourselves). The manual instruction
        // tells the user to upgrade via their AUR helper of choice.
        if (!pickAppImageAsset(pick, asset)) {
            outError = "No AppImage asset in release " + pick.tagName;
            return false;
        }
        method = InstallMethod::Manual;
        manual = "yay -Syu satellite-bin   # or: paru -Syu satellite-bin";
        break;
    case InstallType::Portable:
        // Default to AppImage if present, otherwise .deb, otherwise fail.
        if (pickAppImageAsset(pick, asset)) {
            method = InstallMethod::Manual;
            manual = "Download " + asset.name + " from " + pick.htmlUrl +
                     ", chmod +x, and replace your existing binary.";
        } else if (pickDebAsset(pick, asset)) {
            method = InstallMethod::Manual;
            manual = "Download " + asset.name + " from " + pick.htmlUrl +
                     " and install it with `sudo apt install ./" + asset.name + "`.";
        } else {
            outError = "No Linux asset in release " + pick.tagName;
            return false;
        }
        break;
    }

    out.version = stripTagPrefix(pick.tagName);
    out.channel = pick.prerelease ? "prerelease" : "stable";
    out.assetName = asset.name;
    out.assetUrl = asset.browserUrl;
    out.assetSize = asset.size;
    out.assetSha256 = fetchAssetDigest(pick, asset.name);
    out.releaseNotes = pick.body.size() > 8192 ? pick.body.substr(0, 8192) + "..." : pick.body;
    out.htmlUrl = pick.htmlUrl;
    out.publishedAtEpoch = isoToEpoch(pick.publishedAt);
    out.installMethod = method;
    out.manualInstruction = manual;
    out.available = (out.version != currentVersion);
    return true;
}

bool LinuxUpdaterAdapter::downloadArtifact(
    const UpdateInfo& info, const std::function<void(uint64_t, uint64_t)>& onProgress,
    const std::atomic<bool>* cancel, std::string& outLocalPath, std::string& outError) {
    // ~/.cache/satellite/updates/...
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    std::string dir;
    if (xdg && xdg[0]) {
        dir = std::string(xdg) + "/satellite/updates";
    } else {
        const char* home = std::getenv("HOME");
        dir = std::string(home ? home : "/tmp") + "/.cache/satellite/updates";
    }
    // Best-effort directory creation (`mkdir -p` semantics).
    for (size_t i = 1; i < dir.size(); i++) {
        if (dir[i] == '/') {
            dir[i] = '\0';
            ::mkdir(dir.c_str(), 0700);
            dir[i] = '/';
        }
    }
    ::mkdir(dir.c_str(), 0700);
    std::string dst = dir + "/" + info.assetName;

    if (!httpGetToFile(info.assetUrl, dst, onProgress, cancel, outError)) {
        return false;
    }
    outLocalPath = dst;
    return true;
}

bool LinuxUpdaterAdapter::verifyArtifact(const std::string& localPath, const UpdateInfo& info,
                                         std::string& outError) {
    if (info.assetSha256.empty()) return true;
    std::string actual;
    if (!sha256OfFile(localPath, actual, outError)) return false;
    std::string expected = info.assetSha256;
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](char c) { return static_cast<char>(std::tolower(c)); });
    if (actual != expected) {
        outError = "SHA-256 mismatch: expected " + expected + ", got " + actual;
        return false;
    }
    return true;
}

bool LinuxUpdaterAdapter::applyUpdate(const std::string& localPath, const UpdateInfo& info,
                                       std::string& outError) {
    if (info.installMethod == InstallMethod::Manual) {
        // Nothing to do — the manualInstruction has already been surfaced
        // in the UI. The web "Install" button is hidden in the Manual case.
        return true;
    }
    if (installType_ != InstallType::AppImage) {
        outError = "Self-install not supported for platformId " + platformId_;
        return false;
    }

    // 1. chmod +x the downloaded AppImage.
    if (::chmod(localPath.c_str(), 0755) != 0) {
        outError = std::string("chmod failed: ") + std::strerror(errno);
        return false;
    }

    // 2. Resolve $APPIMAGE — the current running binary path.
    const char* appimg = std::getenv("APPIMAGE");
    if (!appimg || appimg[0] == '\0') {
        outError = "APPIMAGE env var missing (am I really an AppImage?)";
        return false;
    }
    std::string currentPath = appimg;

    // 3. Write a helper script that waits for our PID, swaps, relaunches.
    pid_t pid = getpid();
    char helperPath[] = "/tmp/satellite-update-XXXXXX.sh";
    int fd = ::mkstemps(helperPath, 3);
    if (fd < 0) {
        outError = "mkstemps failed";
        return false;
    }
    // Use printf-piped writes so we don't need a separate fdopen.
    auto append = [&](const std::string& line) {
        ::write(fd, line.c_str(), line.size());
    };
    append("#!/bin/bash\n");
    append("set -e\n");
    append("PID=" + std::to_string(pid) + "\n");
    append("SRC=\"" + localPath + "\"\n");
    append("DST=\"" + currentPath + "\"\n");
    append("for i in $(seq 1 60); do\n");
    append("  if ! kill -0 \"$PID\" 2>/dev/null; then break; fi\n");
    append("  sleep 0.5\n");
    append("done\n");
    // mv across the same filesystem is atomic. Keep a .old copy until the
    // new binary launches successfully — paranoia in case the new one
    // segfaults on startup, the user can manually rename .old back.
    append("if [ -f \"$DST\" ]; then mv -f \"$DST\" \"$DST.old\" || true; fi\n");
    append("mv -f \"$SRC\" \"$DST\"\n");
    append("chmod +x \"$DST\"\n");
    // Re-exec the new AppImage. Detach via setsid so it survives our exit.
    append("setsid \"$DST\" >/dev/null 2>&1 &\n");
    append("rm -f -- \"$0\"\n");
    ::fchmod(fd, 0700);
    ::close(fd);

    // 4. Fork+exec the helper, then signal a clean shutdown.
    pid_t child = fork();
    if (child < 0) {
        outError = std::string("fork failed: ") + std::strerror(errno);
        return false;
    }
    if (child == 0) {
        // Detach from our process group so we survive our parent's exit.
        ::setsid();
        ::execlp("/bin/bash", "bash", helperPath, nullptr);
        ::_exit(127);
    }

    // Tell the main loop to wind down. The Linux build's main.cpp listens
    // for SIGTERM via g_unix_signal_add and translates it to gtk_main_quit.
    ::kill(pid, SIGTERM);
    return true;
}
