// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct GitHubAsset {
    std::string name;
    std::string browserUrl; // direct .exe / .zip / .deb / .AppImage URL
    uint64_t size = 0;
    std::string contentType;
};

struct GitHubRelease {
    std::string tagName; // "v1.2.3" (with the leading v)
    std::string name;
    bool prerelease = false;
    bool draft = false;
    std::string publishedAt; // ISO-8601
    std::string body;        // markdown release notes
    std::string htmlUrl;
    std::vector<GitHubAsset> assets;
};

// Parse GET .../releases/latest. False if too malformed to extract any field.
bool parseGitHubRelease(const std::string& json, GitHubRelease& out);

// Parse GET .../releases (a JSON array). Same return semantics.
bool parseGitHubReleaseList(const std::string& json, std::vector<GitHubRelease>& out);

// "v1.2.3" to "1.2.3"; input unchanged if it doesn't start with v/V.
std::string stripTagPrefix(const std::string& tag);

// ISO-8601 to unix epoch seconds; 0 on parse failure.
int64_t isoToEpoch(const std::string& iso);

// Lowercase 64-char digest for `filename` from a SHA256SUMS body, or "" if
// absent. Tolerates `*` binary-mode markers and surrounding whitespace.
std::string lookupSha256(const std::string& sha256sumsBody, const std::string& filename);
