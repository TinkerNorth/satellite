// SPDX-License-Identifier: LGPL-3.0-or-later

// Parses the GitHub Releases API subset we consume (tolerant: unknown keys are
// ignored, missing/null fields fall back to struct defaults). Backed by
// nlohmann/json via core/json.h.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct GitHubAsset {
    std::string name;       // e.g. "SatelliteSetup-v1.2.3.exe"
    std::string browserUrl; // direct .exe / .zip / .deb / .AppImage URL
    uint64_t size = 0;
    std::string contentType;
};

struct GitHubRelease {
    std::string tagName; // "v1.2.3" (with the leading v)
    std::string name;    // release title (often same as tag)
    bool prerelease = false;
    bool draft = false;
    std::string publishedAt; // ISO-8601 timestamp string
    std::string body;        // markdown release notes
    std::string htmlUrl;     // browser link to the release page
    std::vector<GitHubAsset> assets;
};

// Parse GET .../releases/latest. False means the JSON was too malformed to
// extract any field.
bool parseGitHubRelease(const std::string& json, GitHubRelease& out);

// Parse GET .../releases (a JSON array). Same return semantics.
bool parseGitHubReleaseList(const std::string& json, std::vector<GitHubRelease>& out);

// "v1.2.3" → "1.2.3"; input unchanged if it doesn't start with v/V.
std::string stripTagPrefix(const std::string& tag);

// ISO-8601 "2026-05-13T12:34:56Z" → unix epoch seconds; 0 on parse failure.
int64_t isoToEpoch(const std::string& iso);

// Returns the lowercase 64-char digest for `filename` from a SHA256SUMS body,
// or "" if absent. Tolerates `*` binary-mode markers and surrounding whitespace.
std::string lookupSha256(const std::string& sha256sumsBody, const std::string& filename);
