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
#include <functional>
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

    TEST("a listed file's cache name carries its SHA-256");
    EXPECT_EQ(controllerDbCacheFileName("strings/de.json", "ab12"),
              std::string("controllerdb-ab12-strings-de.json"));

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
    const std::string deName =
        controllerDbCacheFileName("strings/de.json", a.sha256Hex(shipped.files["strings/de.json"]));
    EXPECT(std::filesystem::exists(dir.path + "/" + deName));

    TEST("a cache missing a file its manifest lists reads as absent");
    std::filesystem::remove(dir.path + "/" + deName);
    EXPECT(!a.readCache(back));
}

// A writer that fails its `failAt`-th call (1-based) and writes the rest.
struct FailingWriter {
    int failAt = 0;
    int calls = 0;
    bool operator()(const std::string& path, const std::string& bytes) {
        calls++;
        if (calls == failAt) return false;
        return writeBesideAndRename(path, bytes);
    }
};

// The shipped copy with strings/de.json changed and the manifest saying so.
ControllerDbFiles nextCopy(ControllerDbAdapter& a, const ControllerDbFiles& shipped) {
    ControllerDbFiles next = shipped;
    const std::string oldSha = a.sha256Hex(next.files["strings/de.json"]);
    next.files["strings/de.json"] += " ";
    const std::string newSha = a.sha256Hex(next.files["strings/de.json"]);
    const size_t at = next.manifest.find(oldSha);
    if (at != std::string::npos) next.manifest.replace(at, oldSha.size(), newSha);
    return next;
}

static void test_cache_swap_is_atomic() {
    TempDir dir;
    ControllerDbAdapter good(bundledDir(), dir.path, nullptr, writeBesideAndRename);
    ControllerDbFiles shipped;
    good.readBundled(shipped);
    EXPECT(good.writeCache(shipped));
    const ControllerDbFiles next = nextCopy(good, shipped);
    const size_t writes = next.files.size() + 1;

    for (size_t failAt = 1; failAt <= writes; failAt++) {
        TEST("a cache write that fails at any step leaves the previous cache whole");
        FailingWriter writer;
        writer.failAt = static_cast<int>(failAt);
        ControllerDbAdapter failing(bundledDir(), dir.path, nullptr, std::ref(writer));
        EXPECT(!failing.writeCache(next));
        ControllerDbFiles back;
        EXPECT(good.readCache(back));
        EXPECT(back.manifest == shipped.manifest);
        EXPECT(back.files == shipped.files);
    }

    TEST("a cache write that succeeds swaps the whole set in");
    EXPECT(good.writeCache(next));
    ControllerDbFiles back;
    EXPECT(good.readCache(back));
    EXPECT(back.manifest == next.manifest);
    EXPECT(back.files == next.files);

    TEST("after the swap the previous set's file no longer named is gone");
    const std::string oldDe = controllerDbCacheFileName(
        "strings/de.json", good.sha256Hex(shipped.files.at("strings/de.json")));
    EXPECT(!std::filesystem::exists(dir.path + "/" + oldDe));
    const std::string keptEn = controllerDbCacheFileName(
        "strings/en.json", good.sha256Hex(shipped.files.at("strings/en.json")));
    EXPECT(std::filesystem::exists(dir.path + "/" + keptEn));
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
    test_cache_swap_is_atomic();
    test_fetch_without_transport();
    std::cout << "controller_db_adapter: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
