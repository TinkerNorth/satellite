// SPDX-License-Identifier: LGPL-3.0-or-later
#include "controller_db.h"

#include "json.h"

#include <algorithm>
#include <cctype>

namespace satellite {

namespace {

// The record sections schema 1 requires; a record lacking one is dropped.
constexpr const char* REQUIRED_SECTIONS[] = {"requires", "identity", "input",
                                             "output",   "expect",   "support"};

struct DbParseLabel {
    DbParse result;
    const char* name;
};

constexpr DbParseLabel DB_PARSE_NAMES[] = {
    {DbParse::Ok, "ok"},
    {DbParse::Malformed, "malformed"},
    {DbParse::WrongSchema, "wrongSchema"},
};

struct SnapshotBuildLabel {
    SnapshotBuild result;
    const char* name;
};

constexpr SnapshotBuildLabel SNAPSHOT_BUILD_NAMES[] = {
    {SnapshotBuild::Ok, "ok"},
    {SnapshotBuild::BadManifest, "badManifest"},
    {SnapshotBuild::BadFile, "badFile"},
    {SnapshotBuild::BadRecords, "badRecords"},
    {SnapshotBuild::VersionMismatch, "versionMismatch"},
};

bool isLowerHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

// ^[a-z0-9]+(-[a-z0-9]+)*$
bool isSlug(const std::string& s) {
    if (s.empty() || s.front() == '-' || s.back() == '-') return false;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        const bool doubledDash = c == '-' && i > 0 && s[i - 1] == '-';
        if (!alnum && c != '-') return false;
        if (doubledDash) return false;
    }
    return true;
}

// ^[0-9a-f]{4}:[0-9a-f]{4}$
bool isVidPid(const std::string& s) {
    if (s.size() != 9 || s[4] != ':') return false;
    for (size_t i = 0; i < s.size(); i++) {
        if (i != 4 && !isLowerHex(s[i])) return false;
    }
    return true;
}

bool isSha256Hex(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), isLowerHex);
}

// False when the array is absent or holds anything but vid:pid strings.
bool readIdList(const Json& identity, const char* key, std::vector<std::string>& out) {
    const auto it = identity.find(key);
    if (it == identity.end() || !it->is_array()) return false;
    for (const Json& v : *it) {
        if (!v.is_string() || !isVidPid(v.get<std::string>())) return false;
        out.push_back(v.get<std::string>());
    }
    return true;
}

bool hasRequiredSections(const Json& r) {
    for (const char* section : REQUIRED_SECTIONS) {
        const auto it = r.find(section);
        if (it == r.end() || !it->is_object()) return false;
    }
    long schema = 0;
    return jsonTryInt(r, "schema", schema) && schema == CONTROLLER_DB_SCHEMA;
}

// An issue without an id has no strings to show, so it is skipped alone.
std::vector<ControllerIssue> readIssues(const Json& support) {
    std::vector<ControllerIssue> issues;
    const auto it = support.find("issues");
    if (it == support.end() || !it->is_array()) return issues;
    for (const Json& i : *it) {
        if (!i.is_object()) continue;
        ControllerIssue issue;
        issue.id = jsonStr(i, "id");
        if (!isSlug(issue.id)) continue;
        issue.device = jsonStr(i, "device");
        issue.transport = jsonStr(i, "transport");
        issue.lane = jsonStr(i, "lane");
        issue.status = jsonStr(i, "status");
        issues.push_back(std::move(issue));
    }
    return issues;
}

bool readRecord(const Json& r, ControllerRecord& out) {
    if (!r.is_object() || !hasRequiredSections(r)) return false;
    out.id = jsonStr(r, "id");
    if (!isSlug(out.id)) return false;

    const Json& identity = r["identity"];
    if (!readIdList(identity, "usb", out.usb)) return false;
    if (!readIdList(identity, "bluetooth", out.bluetooth)) return false;

    out.family = jsonStr(jsonObject(r["input"], "protocol"), "ref");
    if (out.family.empty()) return false;

    const Json& support = r["support"];
    const std::string state = jsonStr(jsonObject(support, "verification"), "state");
    if (!parseVerification(state, out.verification)) return false;
    out.issues = readIssues(support);
    return true;
}

void indexRecords(ControllerDb& db) {
    for (size_t i = 0; i < db.records.size(); i++) {
        const ControllerRecord& r = db.records[i];
        for (const std::string& id : r.usb) db.index[id].push_back(i);
        for (const std::string& id : r.bluetooth) db.index[id].push_back(i);
    }
}

DbParse readSchemaAndVersion(const Json& j, long& dbVersion) {
    long schema = 0;
    const bool knownSchema = jsonTryInt(j, "schema", schema) && schema == CONTROLLER_DB_SCHEMA;
    if (!knownSchema) return DbParse::WrongSchema;
    const bool versioned = jsonTryInt(j, "dbVersion", dbVersion) && dbVersion >= 1;
    return versioned ? DbParse::Ok : DbParse::Malformed;
}

bool readManifestFile(const Json& entry, ManifestFile& out) {
    if (!entry.is_object()) return false;
    out.sha256 = jsonStr(entry, "sha256");
    uint64_t bytes = 0;
    const auto it = entry.find("bytes");
    const bool counted = it != entry.end() && it->is_number_unsigned();
    if (counted) bytes = it->get<uint64_t>();
    out.bytes = bytes;
    return isSha256Hex(out.sha256) && counted;
}

SnapshotBuild checkListedFiles(const ControllerDbFiles& files, const ControllerDbManifest& m,
                               const Sha256Fn& sha256) {
    for (const auto& entry : m.files) {
        const auto it = files.files.find(entry.first);
        const std::string* bytes = it == files.files.end() ? nullptr : &it->second;
        if (checkControllerDbFile(entry.second, bytes, sha256) != FileCheck::Ok) {
            return SnapshotBuild::BadFile;
        }
    }
    return SnapshotBuild::Ok;
}

// strings/<locale>.json to <locale>; empty for any other path.
std::string localeOfStringsPath(const std::string& path) {
    const std::string prefix = "strings/";
    const std::string suffix = ".json";
    const bool shaped = path.size() > prefix.size() + suffix.size() &&
                        path.compare(0, prefix.size(), prefix) == 0 &&
                        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
    if (!shaped) return "";
    return path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
}

SnapshotBuild parseListedStrings(const ControllerDbFiles& files, const ControllerDbManifest& m,
                                 ControllerDbStrings& out) {
    for (const auto& entry : m.files) {
        const std::string locale = localeOfStringsPath(entry.first);
        if (locale.empty()) continue;
        ControllerDbStringTable table;
        if (parseControllerDbStrings(files.files.at(entry.first), table) != DbParse::Ok) {
            return SnapshotBuild::BadFile;
        }
        out.byLocale[locale] = std::move(table);
    }
    return SnapshotBuild::Ok;
}

} // namespace

const char* verificationName(Verification state) {
    for (const VerificationName& v : VERIFICATION_NAMES) {
        if (v.state == state) return v.name;
    }
    return "";
}

bool parseVerification(const std::string& name, Verification& out) {
    for (const VerificationName& v : VERIFICATION_NAMES) {
        if (name == v.name) {
            out = v.state;
            return true;
        }
    }
    return false;
}

const ControllerRecord* ControllerDb::find(const std::string& vidPid) const {
    const auto it = index.find(vidPid);
    if (it == index.end() || it->second.empty()) return nullptr;
    return &records[it->second.front()];
}

std::vector<const ControllerRecord*> ControllerDb::findAll(const std::string& vidPid) const {
    std::vector<const ControllerRecord*> out;
    const auto it = index.find(vidPid);
    if (it == index.end()) return out;
    for (const size_t i : it->second) out.push_back(&records[i]);
    return out;
}

const char* dbParseName(DbParse result) {
    for (const DbParseLabel& label : DB_PARSE_NAMES) {
        if (label.result == result) return label.name;
    }
    return "";
}

DbParse parseControllerDb(const std::string& text, ControllerDb& out) {
    out = ControllerDb{};
    Json j;
    if (!jsonParse(text, j) || !j.is_object()) return DbParse::Malformed;
    const DbParse header = readSchemaAndVersion(j, out.dbVersion);
    if (header != DbParse::Ok) return header;
    const auto records = j.find("records");
    if (records == j.end() || !records->is_array()) return DbParse::Malformed;
    for (const Json& r : *records) {
        ControllerRecord record;
        if (readRecord(r, record)) {
            out.records.push_back(std::move(record));
        } else {
            out.rejectedRecords++;
        }
    }
    indexRecords(out);
    return DbParse::Ok;
}

DbParse parseControllerDbStrings(const std::string& text, ControllerDbStringTable& out) {
    out.clear();
    Json j;
    if (!jsonParse(text, j) || !j.is_object()) return DbParse::Malformed;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.value().is_string()) out[it.key()] = it.value().get<std::string>();
    }
    return DbParse::Ok;
}

std::string ControllerDbStrings::lookup(const std::string& locale, const std::string& key) const {
    const auto own = byLocale.find(locale);
    if (own != byLocale.end()) {
        const auto hit = own->second.find(key);
        if (hit != own->second.end()) return hit->second;
    }
    const auto en = byLocale.find(CONTROLLER_DB_FALLBACK_LOCALE);
    if (en == byLocale.end()) return "";
    const auto hit = en->second.find(key);
    return hit == en->second.end() ? "" : hit->second;
}

bool isControllerDbFilePath(const std::string& path) {
    if (path == CONTROLLER_DB_RECORDS) return true;
    const std::string locale = localeOfStringsPath(path);
    for (const char* known : CONTROLLER_DB_LOCALES) {
        if (locale == known) return true;
    }
    return false;
}

DbParse parseControllerDbManifest(const std::string& text, ControllerDbManifest& out) {
    out = ControllerDbManifest{};
    Json j;
    if (!jsonParse(text, j) || !j.is_object()) return DbParse::Malformed;
    const DbParse header = readSchemaAndVersion(j, out.dbVersion);
    if (header != DbParse::Ok) return header;
    out.publishedAt = jsonStr(j, "publishedAt");
    const Json files = jsonObject(j, "files");
    for (auto it = files.begin(); it != files.end(); ++it) {
        if (!isControllerDbFilePath(it.key())) continue;
        ManifestFile file;
        if (!readManifestFile(it.value(), file)) return DbParse::Malformed;
        out.files[it.key()] = file;
    }
    const bool complete =
        out.files.count(CONTROLLER_DB_RECORDS) != 0 && out.files.count("strings/en.json") != 0;
    return complete ? DbParse::Ok : DbParse::Malformed;
}

const char* dbSourceName(DbSource source) {
    switch (source) {
    case DbSource::Bundled:
        return "bundled";
    case DbSource::Cache:
        return "cache";
    }
    return "";
}

FileCheck checkControllerDbFile(const ManifestFile& expected, const std::string* bytes,
                                const Sha256Fn& sha256) {
    if (bytes == nullptr) return FileCheck::Missing;
    if (bytes->size() != expected.bytes) return FileCheck::SizeMismatch;
    if (sha256(*bytes) != expected.sha256) return FileCheck::HashMismatch;
    return FileCheck::Ok;
}

const char* snapshotBuildName(SnapshotBuild result) {
    for (const SnapshotBuildLabel& label : SNAPSHOT_BUILD_NAMES) {
        if (label.result == result) return label.name;
    }
    return "";
}

SnapshotBuild buildControllerDbSnapshot(const ControllerDbFiles& files, DbSource source,
                                        const Sha256Fn& sha256, ControllerDbSnapshot& out) {
    out = ControllerDbSnapshot{};
    out.source = source;
    if (parseControllerDbManifest(files.manifest, out.manifest) != DbParse::Ok) {
        return SnapshotBuild::BadManifest;
    }
    const SnapshotBuild listed = checkListedFiles(files, out.manifest, sha256);
    if (listed != SnapshotBuild::Ok) return listed;
    const std::string& records = files.files.at(CONTROLLER_DB_RECORDS);
    if (parseControllerDb(records, out.db) != DbParse::Ok) return SnapshotBuild::BadRecords;
    if (out.db.dbVersion != out.manifest.dbVersion) return SnapshotBuild::VersionMismatch;
    const SnapshotBuild strings = parseListedStrings(files, out.manifest, out.strings);
    if (strings != SnapshotBuild::Ok) return strings;
    out.manifestSha256 = sha256(files.manifest);
    out.files = files;
    return SnapshotBuild::Ok;
}

std::string controllerDbETag(const std::string& sha256) { return "\"" + sha256 + "\""; }

bool controllerDbMirrorFile(const ControllerDbSnapshot& snap, const std::string& path,
                            MirrorFile& out) {
    if (path == CONTROLLER_DB_MANIFEST) {
        out.bytes = &snap.files.manifest;
        out.etag = controllerDbETag(snap.manifestSha256);
        return true;
    }
    const auto listed = snap.manifest.files.find(path);
    const auto held = snap.files.files.find(path);
    if (listed == snap.manifest.files.end() || held == snap.files.files.end()) return false;
    out.bytes = &held->second;
    out.etag = controllerDbETag(listed->second.sha256);
    return true;
}

bool ifNoneMatchHits(const std::string& header, const std::string& etag) {
    size_t pos = 0;
    while (pos < header.size()) {
        const size_t comma = header.find(',', pos);
        const size_t end = comma == std::string::npos ? header.size() : comma;
        const size_t b = header.find_first_not_of(" \t", pos);
        const size_t e = header.find_last_not_of(" \t", end == 0 ? 0 : end - 1);
        pos = end + 1;
        if (b == std::string::npos || b >= end || e < b) continue;
        std::string tag = header.substr(b, e - b + 1);
        if (tag == "*") return true;
        if (tag.compare(0, 2, "W/") == 0) tag = tag.substr(2);
        if (tag == etag) return true;
    }
    return false;
}

} // namespace satellite
