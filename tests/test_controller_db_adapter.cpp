// SPDX-License-Identifier: LGPL-3.0-or-later

// adapters/controller_db_adapter: the real SHA-256, the copy the binary ships
// held to its own manifest, and the flat-file cache written and read back.
#include "../src/adapters/controller_db_adapter.h"
#include "../src/core/controller_db.h"

#include <sodium.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "test_util.h"

using namespace satellite;

namespace {

bool writeBesideAndRename(const std::string& path, const std::string& bytes) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f << bytes;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// Portable: the suite runs on the Windows lanes too.
struct TempDir {
    std::string path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path p =
            std::filesystem::temp_directory_path() / ("satellite-cdb-" + std::to_string(stamp));
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        path = ec ? "" : p.string();
    }
    ~TempDir() {
        std::error_code ec;
        if (!path.empty()) std::filesystem::remove_all(path, ec);
    }
};

std::string bundledDir() { return std::string(WEB_DIR) + "/controllers/v1"; }

} // namespace

static void test_sha256() {
    TEST("sha256Hex is SHA-256 in lowercase hex (FIPS 180-2 'abc')");
    ControllerDbAdapter a(bundledDir(), "", nullptr, nullptr);
    EXPECT_EQ(a.sha256Hex("abc"),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

static void test_bundled_copy_matches_its_manifest() {
    TEST("the shipped copy's bytes match the SHA-256 and sizes its manifest pins");
    ControllerDbAdapter a(bundledDir(), "", nullptr, nullptr);
    ControllerDbFiles files;
    EXPECT(a.readBundled(files));
    EXPECT_EQ(files.files.size(), size_t{7});
    ControllerDbSnapshot snap;
    const Sha256Fn sha = [&a](const std::string& b) { return a.sha256Hex(b); };
    EXPECT(buildControllerDbSnapshot(files, DbSource::Bundled, sha, snap) == SnapshotBuild::Ok);
    EXPECT_EQ(snap.db.records.size(), size_t{272});
}

static void test_cache_round_trip() {
    TEST("cache names are flat: the manifest path with '/' as '-'");
    EXPECT_EQ(controllerDbCacheName("strings/pt-BR.json"),
              std::string("controllerdb-strings-pt-BR.json"));
    EXPECT_EQ(controllerDbCacheName("manifest.json"), std::string("controllerdb-manifest.json"));

    TEST("no cache reads as absent");
    TempDir dir;
    ControllerDbAdapter a(bundledDir(), dir.path, nullptr, writeBesideAndRename);
    ControllerDbFiles none;
    EXPECT(!a.readCache(none));

    TEST("a written cache reads back byte for byte");
    ControllerDbFiles shipped;
    a.readBundled(shipped);
    EXPECT(a.writeCache(shipped));
    ControllerDbFiles back;
    EXPECT(a.readCache(back));
    EXPECT(back.manifest == shipped.manifest);
    EXPECT(back.files == shipped.files);
    EXPECT(std::filesystem::exists(dir.path + "/controllerdb-strings-de.json"));

    TEST("a cache missing a file its manifest lists reads as absent");
    std::filesystem::remove(dir.path + "/controllerdb-strings-de.json");
    EXPECT(!a.readCache(back));
}

static void test_fetch_without_transport() {
    TEST("with no HTTP GET wired, a fetch fails rather than pretending");
    ControllerDbAdapter a(bundledDir(), "", nullptr, nullptr);
    EXPECT(a.fetch("https://dish.tinkernorth.com/controllers/v1/manifest.json", "").outcome ==
           DbFetch::Failed);
}

int main() {
    if (sodium_init() < 0) return 1;
    test_sha256();
    test_bundled_copy_matches_its_manifest();
    test_cache_round_trip();
    test_fetch_without_transport();
    std::cout << "controller_db_adapter: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
