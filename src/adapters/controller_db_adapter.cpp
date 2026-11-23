// SPDX-License-Identifier: LGPL-3.0-or-later
#include "controller_db_adapter.h"

#include "core/hex.h"

#include <sodium.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <iterator>
#include <utility>

using satellite::ControllerDbFiles;
using satellite::ControllerDbManifest;

namespace {

constexpr const char* CACHE_PREFIX = "controllerdb-";

bool readWhole(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

using FilePathOf = std::function<std::string(const std::string&, const satellite::ManifestFile&)>;

// The manifest at `manifestPath`, then every file it lists, each read by
// `pathOf`. False when the manifest or a listed file is missing; whether the
// bytes match is the snapshot's question.
bool readCopy(const std::string& manifestPath, const FilePathOf& pathOf, ControllerDbFiles& out) {
    out = ControllerDbFiles{};
    if (!readWhole(manifestPath, out.manifest)) return false;
    ControllerDbManifest manifest;
    if (satellite::parseControllerDbManifest(out.manifest, manifest) != satellite::DbParse::Ok) {
        return false;
    }
    for (const auto& entry : manifest.files) {
        std::string bytes;
        if (!readWhole(pathOf(entry.first, entry.second), bytes)) return false;
        out.files[entry.first] = std::move(bytes);
    }
    return true;
}

// The cache files a manifest's text names, or none when it does not parse.
std::vector<std::string> cacheNamesOf(const std::string& manifestText) {
    std::vector<std::string> names;
    ControllerDbManifest manifest;
    if (satellite::parseControllerDbManifest(manifestText, manifest) != satellite::DbParse::Ok) {
        return names;
    }
    for (const auto& entry : manifest.files) {
        names.push_back(controllerDbCacheFileName(entry.first, entry.second.sha256));
    }
    return names;
}

} // namespace

std::string controllerDbCacheName(const std::string& manifestPath) {
    std::string name = CACHE_PREFIX + manifestPath;
    for (char& c : name) {
        if (c == '/') c = '-';
    }
    return name;
}

std::string controllerDbCacheFileName(const std::string& manifestPath, const std::string& sha256) {
    return controllerDbCacheName(sha256 + "-" + manifestPath);
}

std::string controllerDbCacheDirFor(const std::string& configFilePath) {
    const size_t slash = configFilePath.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : configFilePath.substr(0, slash);
}

ControllerDbAdapter::ControllerDbAdapter(std::string bundledDir, std::string cacheDir,
                                         DbHttpGet get, DbWriteFile write)
    : bundledDir_(std::move(bundledDir)), cacheDir_(std::move(cacheDir)), get_(std::move(get)),
      write_(std::move(write)) {}

DbFetchResponse ControllerDbAdapter::fetch(const std::string& url, const std::string& ifNoneMatch) {
    if (!get_) return DbFetchResponse{};
    return get_(url, ifNoneMatch);
}

std::string ControllerDbAdapter::sha256Hex(const std::string& bytes) {
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    return hexEncode(digest, sizeof(digest));
}

bool ControllerDbAdapter::readBundled(ControllerDbFiles& out) {
    return readCopy(
        bundledDir_ + "/" + satellite::CONTROLLER_DB_MANIFEST,
        [this](const std::string& p, const satellite::ManifestFile&) {
            return bundledDir_ + "/" + p;
        },
        out);
}

std::string ControllerDbAdapter::cacheManifestPath() const {
    return cacheDir_ + "/" + controllerDbCacheName(satellite::CONTROLLER_DB_MANIFEST);
}

bool ControllerDbAdapter::readCache(ControllerDbFiles& out) {
    return readCopy(
        cacheManifestPath(),
        [this](const std::string& p, const satellite::ManifestFile& f) {
            return cacheDir_ + "/" + controllerDbCacheFileName(p, f.sha256);
        },
        out);
}

// Contract §6.3: each file is written under a name its SHA-256 fixes, so no
// write touches a file the cache's manifest names, and the manifest's own
// write-beside-and-rename is the one step that swaps the set in. A failure
// before it leaves the previous cache whole. The previous set's files no
// longer named are removed after the swap; one that will not go stays as an
// unread file.
bool ControllerDbAdapter::writeCache(const ControllerDbFiles& files) {
    std::string previous;
    const bool hadCache = readWhole(cacheManifestPath(), previous);
    ControllerDbManifest manifest;
    if (satellite::parseControllerDbManifest(files.manifest, manifest) != satellite::DbParse::Ok) {
        return false;
    }
    for (const auto& entry : manifest.files) {
        const auto bytes = files.files.find(entry.first);
        if (bytes == files.files.end()) return false;
        const std::string path =
            cacheDir_ + "/" + controllerDbCacheFileName(entry.first, entry.second.sha256);
        if (!write_(path, bytes->second)) return false;
    }
    if (!write_(cacheManifestPath(), files.manifest)) return false;
    if (hadCache) removeUnnamed(cacheNamesOf(previous), cacheNamesOf(files.manifest));
    return true;
}

void ControllerDbAdapter::removeUnnamed(const std::vector<std::string>& before,
                                        const std::vector<std::string>& now) const {
    for (const std::string& name : before) {
        const bool stillNamed = std::find(now.begin(), now.end(), name) != now.end();
        if (stillNamed) continue;
        const std::string path = cacheDir_ + "/" + name;
        if (std::remove(path.c_str()) != 0) continue;
    }
}

int64_t ControllerDbAdapter::nowEpoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
