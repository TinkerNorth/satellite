// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/github_release.h"

#include <iostream>
#include <string>
#include <vector>

#include "test_util.h"

// ---- parseGitHubRelease ------------------------------------------------------

static void test_parse_full_release() {
    TEST("parseGitHubRelease — extracts every field + nested assets");
    const std::string json = R"({
      "tag_name": "v1.2.3",
      "name": "Release 1.2.3",
      "prerelease": false,
      "draft": false,
      "published_at": "2026-05-13T12:34:56Z",
      "body": "notes here",
      "html_url": "https://github.com/x/y/releases/tag/v1.2.3",
      "assets": [
        {"name": "SatelliteSetup-v1.2.3.exe",
         "browser_download_url": "https://example/SatelliteSetup-v1.2.3.exe",
         "size": 4096, "content_type": "application/octet-stream"},
        {"name": "SHA256SUMS", "browser_download_url": "https://example/SHA256SUMS",
         "size": 200, "content_type": "text/plain"}
      ]
    })";
    GitHubRelease r;
    EXPECT(parseGitHubRelease(json, r));
    EXPECT_EQ(r.tagName, std::string("v1.2.3"));
    EXPECT_EQ(r.name, std::string("Release 1.2.3"));
    EXPECT(!r.prerelease);
    EXPECT(!r.draft);
    EXPECT_EQ(r.publishedAt, std::string("2026-05-13T12:34:56Z"));
    EXPECT_EQ(r.body, std::string("notes here"));
    EXPECT_EQ(r.assets.size(), (size_t)2);
    EXPECT_EQ(r.assets[0].name, std::string("SatelliteSetup-v1.2.3.exe"));
    EXPECT_EQ(r.assets[0].size, (uint64_t)4096);
    EXPECT_EQ(r.assets[1].name, std::string("SHA256SUMS"));
}

static void test_parse_null_optional_fields() {
    TEST("parseGitHubRelease — tolerates null name/body/published_at");
    const std::string json = R"({"tag_name":"v2.0.0","name":null,"body":null,
      "published_at":null,"prerelease":true,"assets":[]})";
    GitHubRelease r;
    EXPECT(parseGitHubRelease(json, r));
    EXPECT_EQ(r.tagName, std::string("v2.0.0"));
    EXPECT(r.name.empty());
    EXPECT(r.body.empty());
    EXPECT(r.prerelease);
    EXPECT_EQ(r.assets.size(), (size_t)0);
}

static void test_parse_malformed_returns_false() {
    TEST("parseGitHubRelease — malformed JSON returns false");
    GitHubRelease r;
    EXPECT(!parseGitHubRelease("not json at all", r));
    EXPECT(!parseGitHubRelease("{\"tag_name\": ", r)); // truncated
    EXPECT(!parseGitHubRelease("", r));
}

// ---- parseGitHubReleaseList --------------------------------------------------

static void test_parse_list() {
    TEST("parseGitHubReleaseList — array of releases, and empty array");
    std::vector<GitHubRelease> out;
    EXPECT(parseGitHubReleaseList(R"([{"tag_name":"v1.0.0"},{"tag_name":"v1.1.0"}])", out));
    EXPECT_EQ(out.size(), (size_t)2);
    EXPECT_EQ(out[1].tagName, std::string("v1.1.0"));

    std::vector<GitHubRelease> empty;
    EXPECT(parseGitHubReleaseList("[]", empty));
    EXPECT_EQ(empty.size(), (size_t)0);

    std::vector<GitHubRelease> bad;
    EXPECT(!parseGitHubReleaseList("[{", bad));
}

// ---- stripTagPrefix ----------------------------------------------------------

static void test_strip_tag_prefix() {
    TEST("stripTagPrefix — drops a leading v/V, leaves the rest untouched");
    EXPECT_EQ(stripTagPrefix("v1.2.3"), std::string("1.2.3"));
    EXPECT_EQ(stripTagPrefix("V1.2.3"), std::string("1.2.3"));
    EXPECT_EQ(stripTagPrefix("1.2.3"), std::string("1.2.3"));
    EXPECT_EQ(stripTagPrefix(""), std::string(""));
}

// ---- isoToEpoch --------------------------------------------------------------

static void test_iso_to_epoch() {
    TEST("isoToEpoch — known UTC vectors, 0 on parse failure");
    EXPECT_EQ(isoToEpoch("1970-01-01T00:00:00Z"), (int64_t)0);
    EXPECT_EQ(isoToEpoch("2021-01-01T00:00:00Z"), (int64_t)1609459200);
    EXPECT_EQ(isoToEpoch("2000-01-01T00:00:00Z"), (int64_t)946684800);
    EXPECT_EQ(isoToEpoch("short"), (int64_t)0);
    EXPECT_EQ(isoToEpoch("not-a-timestamp-xx"), (int64_t)0);
}

// ---- lookupSha256 ------------------------------------------------------------

static void test_lookup_sha256() {
    TEST("lookupSha256 — hit, binary-mode marker, lowercasing, miss");
    const std::string a(64, 'a');
    const std::string b(64, 'A'); // uppercase digest, expect lowercased return
    const std::string body = a + "  SatelliteSetup.exe\n" + b + " *Other.zip\n";

    EXPECT_EQ(lookupSha256(body, "SatelliteSetup.exe"), a);
    EXPECT_EQ(lookupSha256(body, "Other.zip"), std::string(64, 'a')); // binary marker + lowercased
    EXPECT_EQ(lookupSha256(body, "Missing.exe"), std::string(""));
    EXPECT_EQ(lookupSha256("tooshort  x\n", "x"), std::string(""));
}

int main() {
    std::cout << "Running github_release tests...\n\n";
    test_parse_full_release();
    test_parse_null_optional_fields();
    test_parse_malformed_returns_false();
    test_parse_list();
    test_strip_tag_prefix();
    test_iso_to_epoch();
    test_lookup_sha256();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    if (g_fail > 0) {
        std::cout << "  STATUS: FAIL\n";
        return 1;
    }
    std::cout << "  STATUS: ALL PASSED\n";
    return 0;
}
