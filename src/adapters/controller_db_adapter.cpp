// SPDX-License-Identifier: LGPL-3.0-or-later
#include "controller_db_adapter.h"

#include "core/hex.h"

#include <sodium.h>

#include <chrono>
#include <fstream>
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

// The manifest, then every file it lists, each read by `pathOf`. False when
// the manifest or a listed file is missing; whether the bytes match is the
// snapshot's question.
bool readCopy(const std::function<std::string(const std::string&)>& pathOf,
              ControllerDbFiles& out) {
    out = ControllerDbFiles{};
    if (!readWhole(pathOf(satellite::CONTROLLER_DB_MANIFEST), out.manifest)) return false;
    ControllerDbManifest manifest;
    if (satellite::parseControllerDbManifest(out.manifest, manifest) != satellite::DbParse::Ok) {
        return false;
    }
    for (const auto& entry : manifest.files) {
        std::string bytes;
        if (!readWhole(pathOf(entry.first), bytes)) return false;
        out.files[entry.first] = std::move(bytes);
    }
    return true;
}

} // namespace

std::string controllerDbCacheName(const std::string& manifestPath) {
    std::string name = CACHE_PREFIX + manifestPath;
    for (char& c : name) {
        if (c == '/') c = '-';
    }
    return name;
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
    return readCopy([this](const std::string& p) { return bundledDir_ + "/" + p; }, out);
}

bool ControllerDbAdapter::readCache(ControllerDbFiles& out) {
    return readCopy(
        [this](const std::string& p) { return cacheDir_ + "/" + controllerDbCacheName(p); }, out);
}

bool ControllerDbAdapter::writeCache(const ControllerDbFiles& files) {
    for (const auto& file : files.files) {
        if (!write_(cacheDir_ + "/" + controllerDbCacheName(file.first), file.second)) return false;
    }
    return write_(cacheDir_ + "/" + controllerDbCacheName(satellite::CONTROLLER_DB_MANIFEST),
                  files.manifest);
}

int64_t ControllerDbAdapter::nowEpoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
