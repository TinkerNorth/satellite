// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * core/github_release.h — Minimal parser for the GitHub Releases API
 *                         response shape that the OTA updater consumes.
 *
 * Pure types + a tiny recursive-descent JSON parser, narrowed to the
 * release subset we care about. Shared across the Windows / macOS /
 * Linux updater adapters so each only owns its platform IO.
 *
 * Why a custom parser?  The project deliberately avoids pulling in a
 * JSON library (no nlohmann/json, no rapidjson) — the rest of the
 * codebase uses hand-rolled jsonGetString helpers. For GitHub the
 * release body can contain arbitrary text including `"key":"value"`
 * substrings, so the shallow key-find approach used elsewhere would
 * mis-match. The parser below is ~150 LOC and tolerant of the
 * fields we don't read.
 */
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

// Parse the response body of GET /repos/{owner}/{repo}/releases/latest.
// Returns true on success and fills `out`. False means the JSON was
// malformed enough that no fields could be extracted.
bool parseGitHubRelease(const std::string& json, GitHubRelease& out);

// Parse the response body of GET /repos/{owner}/{repo}/releases
// (a JSON array of release objects). Same return semantics.
bool parseGitHubReleaseList(const std::string& json, std::vector<GitHubRelease>& out);

// Convert tag "v1.2.3" → version "1.2.3". Returns the input unchanged if
// it doesn't start with v/V.
std::string stripTagPrefix(const std::string& tag);

// Convert ISO-8601 "2026-05-13T12:34:56Z" → unix epoch seconds. Returns 0
// on parse failure.
int64_t isoToEpoch(const std::string& iso);

// Find a single line in a SHA256SUMS body matching the given filename.
// Returns the hex digest (lowercase, 64 chars) or "" if absent.
// Tolerates `*` mode markers (sha256sum --binary) and arbitrary
// surrounding whitespace.
std::string lookupSha256(const std::string& sha256sumsBody, const std::string& filename);
