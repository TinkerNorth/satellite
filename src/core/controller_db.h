// SPDX-License-Identifier: LGPL-3.0-or-later

// The fleet's controller database (docs/ARCHITECTURE.md "Controller
// database"): the schema-1 files parsed into what Satellite reads of them.
// Pure: text in, immutable records out; the bundle, the cache and the fetch
// live in adapters/controller_db_adapter.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace satellite {

inline constexpr long CONTROLLER_DB_SCHEMA = 1;

// The six locales every strings file set carries; `en` is complete and the
// fallback for the rest.
inline constexpr const char* CONTROLLER_DB_FALLBACK_LOCALE = "en";
inline constexpr const char* CONTROLLER_DB_LOCALES[] = {"en", "bs", "de", "es", "fr", "pt-BR"};

// input.protocol.ref's vocabulary (contract §4.1), the families requires.families
// may name; schema 1 defines no op.
inline constexpr const char* CONTROLLER_DB_FAMILIES[] = {
    "xinput360", "xinput360Wireless", "xboxOneGip",   "dualshock4",
    "dualsense", "switchProUsb",      "switchN64Usb", "stadia",
    "hid",       "steamController",   "steamTriton",  "steamControllerBle",
};

inline constexpr const char* CONTROLLER_DB_MANIFEST = "manifest.json";
inline constexpr const char* CONTROLLER_DB_RECORDS = "controllers.json";

enum class Verification : uint8_t { Verified, Imported, Crowd, Guessed, Flagged };

struct VerificationName {
    Verification state;
    const char* name;
};

inline constexpr VerificationName VERIFICATION_NAMES[] = {
    {Verification::Verified, "verified"}, {Verification::Imported, "imported"},
    {Verification::Crowd, "crowd"},       {Verification::Guessed, "guessed"},
    {Verification::Flagged, "flagged"},
};

const char* verificationName(Verification state);
bool parseVerification(const std::string& name, Verification& out);

// An issue carries no prose: its title and body are strings under
// issue.<id>.title and issue.<id>.body.
struct ControllerIssue {
    std::string id;
    std::string device;
    std::string transport;
    std::string lane;
    std::string status;
};

struct ControllerRecord {
    std::string id;
    std::vector<std::string> usb;
    std::vector<std::string> bluetooth;
    // input.protocol.ref, the parser family.
    std::string family;
    Verification verification = Verification::Imported;
    std::vector<ControllerIssue> issues;
    // requires names a family or op this build lacks (contract §6.6): the
    // record is named, its data is not applied, and the build's own row for
    // its ids stays in force.
    bool inert = false;
};

struct ControllerDb {
    long dbVersion = 0;
    std::vector<ControllerRecord> records;
    // Records dropped for a missing or malformed required section.
    size_t rejectedRecords = 0;
    // vid:pid (usb and bluetooth alike) to indices into `records`, in file
    // order: one id can name more than one record.
    std::unordered_map<std::string, std::vector<size_t>> index;

    // The first record in file order that carries `vidPid`, or null.
    const ControllerRecord* find(const std::string& vidPid) const;
    std::vector<const ControllerRecord*> findAll(const std::string& vidPid) const;
};

enum class DbParse : uint8_t { Ok, Malformed, WrongSchema };

const char* dbParseName(DbParse result);

DbParse parseControllerDb(const std::string& text, ControllerDb& out);

// One locale's flat key-to-string object.
using ControllerDbStringTable = std::unordered_map<std::string, std::string>;

DbParse parseControllerDbStrings(const std::string& text, ControllerDbStringTable& out);

struct ControllerDbStrings {
    std::map<std::string, ControllerDbStringTable> byLocale;

    // The locale's string, else `en`'s, else empty.
    std::string lookup(const std::string& locale, const std::string& key) const;
};

struct ManifestFile {
    std::string sha256;
    uint64_t bytes = 0;
};

struct ControllerDbManifest {
    long dbVersion = 0;
    std::string publishedAt;
    // Only the paths this build fetches and mirrors, keyed by manifest path.
    std::map<std::string, ManifestFile> files;
};

// The manifest's paths become file names on disk, so only the ones the
// contract defines are read; any other entry is ignored, never fetched.
bool isControllerDbFilePath(const std::string& path);

DbParse parseControllerDbManifest(const std::string& text, ControllerDbManifest& out);

// The raw bytes of one published copy, as served: the manifest and each file
// it lists, keyed by manifest path.
struct ControllerDbFiles {
    std::string manifest;
    std::map<std::string, std::string> files;
};

enum class DbSource : uint8_t { Bundled, Cache };

const char* dbSourceName(DbSource source);

// Hex SHA-256 of a byte string, injected so the core stays libsodium-free.
using Sha256Fn = std::function<std::string(const std::string&)>;

enum class FileCheck : uint8_t { Ok, Missing, SizeMismatch, HashMismatch };

FileCheck checkControllerDbFile(const ManifestFile& expected, const std::string* bytes,
                                const Sha256Fn& sha256);

// Everything Satellite reads from one copy, built once and never mutated.
struct ControllerDbSnapshot {
    DbSource source = DbSource::Bundled;
    ControllerDbFiles files;
    ControllerDbManifest manifest;
    std::string manifestSha256;
    // What the mirror serves as the manifest: the published one, less any
    // file this build does not hold, so it never lists a file it cannot serve.
    std::string mirrorManifest;
    std::string mirrorManifestSha256;
    ControllerDb db;
    ControllerDbStrings strings;
};

enum class SnapshotBuild : uint8_t { Ok, BadManifest, BadFile, BadRecords, VersionMismatch };

const char* snapshotBuildName(SnapshotBuild result);

// Every file the manifest lists checked against it, then parsed; the records'
// dbVersion must be the manifest's.
SnapshotBuild buildControllerDbSnapshot(const ControllerDbFiles& files, DbSource source,
                                        const Sha256Fn& sha256, ControllerDbSnapshot& out);

// The strong validator the mirror serves for a file: its SHA-256, quoted.
std::string controllerDbETag(const std::string& sha256);

// What GET /api/controllers/v1/<path> serves from a snapshot: the manifest or
// a file it lists, with its validator. False for any other path.
struct MirrorFile {
    const std::string* bytes = nullptr;
    std::string etag;
};

bool controllerDbMirrorFile(const ControllerDbSnapshot& snap, const std::string& path,
                            MirrorFile& out);

// RFC 9110 If-None-Match against one strong validator: "*", or any listed tag,
// weak (W/) or not, that equals it.
bool ifNoneMatchHits(const std::string& header, const std::string& etag);

} // namespace satellite
