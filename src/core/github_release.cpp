// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/github_release.h"

#include "core/json.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <sstream>

using satellite::Json;
using satellite::jsonBool;
using satellite::jsonParse;
using satellite::jsonStr;

namespace {

GitHubAsset readAsset(const Json& a) {
    GitHubAsset out;
    out.name = jsonStr(a, "name");
    out.browserUrl = jsonStr(a, "browser_download_url");
    // GitHub's "size" is an integer; tolerate any numeric form.
    if (auto it = a.find("size"); it != a.end() && it->is_number()) {
        out.size = it->get<uint64_t>();
    }
    out.contentType = jsonStr(a, "content_type");
    return out;
}

// Tolerant by design: every field falls back to its struct default when absent
// or null (a release "name"/"body"/"published_at" is null when unset in the UI).
GitHubRelease readRelease(const Json& r) {
    GitHubRelease out;
    out.tagName = jsonStr(r, "tag_name");
    out.name = jsonStr(r, "name");
    out.prerelease = jsonBool(r, "prerelease");
    out.draft = jsonBool(r, "draft");
    out.publishedAt = jsonStr(r, "published_at");
    out.body = jsonStr(r, "body");
    out.htmlUrl = jsonStr(r, "html_url");
    if (auto it = r.find("assets"); it != r.end() && it->is_array()) {
        for (const auto& a : *it) {
            if (a.is_object()) out.assets.push_back(readAsset(a));
        }
    }
    return out;
}

} // namespace

bool parseGitHubRelease(const std::string& json, GitHubRelease& out) {
    Json j;
    if (!jsonParse(json, j) || !j.is_object()) return false;
    out = readRelease(j);
    return true;
}

bool parseGitHubReleaseList(const std::string& json, std::vector<GitHubRelease>& out) {
    Json j;
    if (!jsonParse(json, j) || !j.is_array()) return false;
    for (const auto& e : j) {
        if (e.is_object()) out.push_back(readRelease(e));
    }
    return true;
}

std::string stripTagPrefix(const std::string& tag) {
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) return tag.substr(1);
    return tag;
}

int64_t isoToEpoch(const std::string& iso) {
    if (iso.size() < 19) return 0;
    struct tm t{};
    int y, mo, d, h, mi, se;
#ifdef _MSC_VER
    if (sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
#else
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
#endif
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = se;
    // ISO string is UTC ("Z"). timegm() is non-portable; use _mkgmtime on
    // Windows. Display-only, so an off-by-DST doesn't affect update logic.
#if defined(_WIN32)
    return _mkgmtime(&t);
#else
    return timegm(&t);
#endif
}

std::string lookupSha256(const std::string& body, const std::string& filename) {
    std::stringstream ss(body);
    std::string line;
    while (std::getline(ss, line)) {
        // Format: "<64-hex>  <filename>" or "<64-hex> *<filename>" (binary mode).
        if (line.size() < 66) continue;
        size_t i = 0;
        while (i < line.size() && std::isxdigit(static_cast<unsigned char>(line[i]))) ++i;
        if (i != 64) continue;
        std::string digest = line.substr(0, 64);
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i < line.size() && line[i] == '*') ++i; // binary-mode marker
        std::string name = line.substr(i);
        while (!name.empty() &&
               (name.back() == '\r' || name.back() == '\n' || name.back() == ' ')) {
            name.pop_back();
        }
        if (name == filename) {
            std::transform(digest.begin(), digest.end(), digest.begin(),
                           [](char c) { return static_cast<char>(std::tolower(c)); });
            return digest;
        }
    }
    return "";
}
