// SPDX-License-Identifier: LGPL-3.0-or-later

// IControllerDbPort over the files the binary ships (web/controllers/v1), a
// cache of flat files in the config directory, libsodium's SHA-256, and the
// platform's HTTP GET, which main.cpp hands in.
#pragma once

#include "core/ports.h"

#include <functional>
#include <string>
#include <vector>

using DbHttpGet =
    std::function<DbFetchResponse(const std::string& url, const std::string& ifNoneMatch)>;

// Writes `bytes` beside `path` and renames it over: the platform's
// atomicWriteFile in production.
using DbWriteFile = std::function<bool(const std::string& path, const std::string& bytes)>;

// The cache file for a manifest path: "controllerdb-" and the path with each
// '/' as '-', so the cache needs no directory of its own.
std::string controllerDbCacheName(const std::string& manifestPath);

// A listed file's cache name, fixed by its SHA-256 as the manifest gives it
// ("controllerdb-<sha256>-strings-de.json"): a new copy's files never land on
// the ones the cache's manifest still names.
std::string controllerDbCacheFileName(const std::string& manifestPath, const std::string& sha256);

// The directory a config file lives in: where the cache is kept.
std::string controllerDbCacheDirFor(const std::string& configFilePath);

class ControllerDbAdapter : public IControllerDbPort {
  public:
    ControllerDbAdapter(std::string bundledDir, std::string cacheDir, DbHttpGet get,
                        DbWriteFile write);

    DbFetchResponse fetch(const std::string& url, const std::string& ifNoneMatch) override;
    std::string sha256Hex(const std::string& bytes) override;
    bool readBundled(satellite::ControllerDbFiles& out) override;
    bool readCache(satellite::ControllerDbFiles& out) override;
    bool writeCache(const satellite::ControllerDbFiles& files) override;
    int64_t nowEpoch() override;

  private:
    std::string cacheManifestPath() const;
    void removeUnnamed(const std::vector<std::string>& before,
                       const std::vector<std::string>& now) const;

    const std::string bundledDir_;
    const std::string cacheDir_;
    const DbHttpGet get_;
    const DbWriteFile write_;
};
