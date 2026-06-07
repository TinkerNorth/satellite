// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/github_release.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <sstream>

namespace {

// Tiny JSON parser subset: objects, arrays, strings, numbers, bool, null;
// skips unknown values. Throws on malformation; callers catch and fall back
// to "false" returns.
class JsonError : public std::exception {
  public:
    const char* what() const noexcept override { return "json parse error"; }
};

class JsonScanner {
  public:
    explicit JsonScanner(const std::string& src) : s_(src) {}

    void skipWs() {
        while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_;
    }

    char peek() {
        skipWs();
        if (p_ >= s_.size()) throw JsonError{};
        return s_[p_];
    }

    char eat() {
        char c = peek();
        ++p_;
        return c;
    }

    void expect(char c) {
        if (eat() != c) throw JsonError{};
    }

    bool maybe(char c) {
        skipWs();
        if (p_ < s_.size() && s_[p_] == c) {
            ++p_;
            return true;
        }
        return false;
    }

    std::string readString() {
        expect('"');
        std::string out;
        while (p_ < s_.size()) {
            char c = s_[p_++];
            if (c == '"') return out;
            if (c == '\\' && p_ < s_.size()) {
                char esc = s_[p_++];
                switch (esc) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    // 4-hex codepoint → UTF-8; surrogate pairs degrade to U+FFFD.
                    if (p_ + 4 > s_.size()) throw JsonError{};
                    unsigned cp = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = s_[p_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9')
                            cp |= h - '0';
                        else if (h >= 'a' && h <= 'f')
                            cp |= 10 + h - 'a';
                        else if (h >= 'A' && h <= 'F')
                            cp |= 10 + h - 'A';
                        else
                            throw JsonError{};
                    }
                    if (cp < 0x80) {
                        out += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                        out += "\xEF\xBF\xBD"; // surrogate half → U+FFFD
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default:
                    throw JsonError{};
                }
            } else {
                out += c;
            }
        }
        throw JsonError{};
    }

    // GitHub's "size" field is always an integer in practice; fractional/
    // exponent chars are consumed but strtoll truncates at the first non-digit.
    int64_t readNumber() {
        skipWs();
        size_t start = p_;
        if (p_ < s_.size() && (s_[p_] == '-' || s_[p_] == '+')) ++p_;
        while (p_ < s_.size() &&
               (std::isdigit(static_cast<unsigned char>(s_[p_])) || s_[p_] == '.' ||
                s_[p_] == 'e' || s_[p_] == 'E' || s_[p_] == '+' || s_[p_] == '-')) {
            ++p_;
        }
        if (start == p_) throw JsonError{};
        std::string tok = s_.substr(start, p_ - start);
        return static_cast<int64_t>(std::strtoll(tok.c_str(), nullptr, 10));
    }

    bool readBool() {
        skipWs();
        if (s_.compare(p_, 4, "true") == 0) {
            p_ += 4;
            return true;
        }
        if (s_.compare(p_, 5, "false") == 0) {
            p_ += 5;
            return false;
        }
        throw JsonError{};
    }

    void readNull() {
        skipWs();
        if (s_.compare(p_, 4, "null") == 0) {
            p_ += 4;
            return;
        }
        throw JsonError{};
    }

    void skipValue() {
        char c = peek();
        if (c == '"') {
            (void)readString();
        } else if (c == 't' || c == 'f') {
            (void)readBool();
        } else if (c == 'n') {
            readNull();
        } else if (c == '{') {
            expect('{');
            if (!maybe('}')) {
                do {
                    (void)readString();
                    skipWs();
                    expect(':');
                    skipValue();
                } while (maybe(','));
                expect('}');
            }
        } else if (c == '[') {
            expect('[');
            if (!maybe(']')) {
                do { skipValue(); } while (maybe(','));
                expect(']');
            }
        } else {
            (void)readNumber();
        }
    }

  private:
    const std::string& s_;
    size_t p_ = 0;
};

GitHubAsset readAsset(JsonScanner& js) {
    GitHubAsset a;
    js.expect('{');
    if (js.maybe('}')) return a;
    do {
        std::string key = js.readString();
        js.skipWs();
        js.expect(':');
        js.skipWs();
        if (key == "name") {
            a.name = js.readString();
        } else if (key == "browser_download_url") {
            a.browserUrl = js.readString();
        } else if (key == "size") {
            a.size = static_cast<uint64_t>(js.readNumber());
        } else if (key == "content_type") {
            a.contentType = js.readString();
        } else {
            js.skipValue();
        }
    } while (js.maybe(','));
    js.expect('}');
    return a;
}

GitHubRelease readRelease(JsonScanner& js) {
    GitHubRelease r;
    js.expect('{');
    if (js.maybe('}')) return r;
    do {
        std::string key = js.readString();
        js.skipWs();
        js.expect(':');
        js.skipWs();
        if (key == "tag_name") {
            r.tagName = js.readString();
        } else if (key == "name") {
            // A release "name" is null when unset in the GitHub UI.
            if (js.peek() == '"')
                r.name = js.readString();
            else
                js.readNull();
        } else if (key == "prerelease") {
            r.prerelease = js.readBool();
        } else if (key == "draft") {
            r.draft = js.readBool();
        } else if (key == "published_at") {
            if (js.peek() == '"')
                r.publishedAt = js.readString();
            else
                js.readNull();
        } else if (key == "body") {
            if (js.peek() == '"')
                r.body = js.readString();
            else
                js.readNull();
        } else if (key == "html_url") {
            r.htmlUrl = js.readString();
        } else if (key == "assets") {
            js.expect('[');
            if (!js.maybe(']')) {
                do {
                    js.skipWs();
                    r.assets.push_back(readAsset(js));
                } while (js.maybe(','));
                js.expect(']');
            }
        } else {
            js.skipValue();
        }
    } while (js.maybe(','));
    js.expect('}');
    return r;
}

} // namespace

bool parseGitHubRelease(const std::string& json, GitHubRelease& out) {
    try {
        JsonScanner js(json);
        out = readRelease(js);
        return true;
    } catch (...) { return false; }
}

bool parseGitHubReleaseList(const std::string& json, std::vector<GitHubRelease>& out) {
    try {
        JsonScanner js(json);
        js.expect('[');
        if (js.maybe(']')) return true;
        do { out.push_back(readRelease(js)); } while (js.maybe(','));
        js.expect(']');
        return true;
    } catch (...) { return false; }
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
