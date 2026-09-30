// SPDX-License-Identifier: LGPL-3.0-or-later

// Client API (sender-facing HTTPS) route handlers. Each handler is a named
// function and registerClientRoutes is the table that binds paths to them;
// the JSON builders used only by this surface stay file-static.
#include "routes_client.h"
#include "routes_common.h"
#include "crypto.h"
#include "config.h"
#include "pairing.h"
#include "pairing_keys.h"
#include "pairing_service.h"
#include "session_crypto.h"
#include "core/catalog.h"
#include "core/descriptor_json.h"
#include "core/hex.h"
#include "core/json.h"
#include "core/session_service.h"
#include "core/version.h"
#include "app/wire_stats.h"

#include <sodium.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

using satellite::capsJsonObj;
using satellite::Json;
using satellite::jsonBool;
using satellite::jsonDump;
using satellite::jsonObject;
using satellite::JsonOut;
using satellite::jsonRequestBody;
using satellite::jsonStr;
using satellite::jsonTryInt;
using satellite::parseControllerDescriptors;
using satellite::parseDescriptorObject;
using satellite::saturateToByte;

using Request = httplib::Request;
using Response = httplib::Response;

struct ClientAuth {
    std::string deviceId;
    PairedDevice device; // copy-by-value under g_configMtx, never a pointer
    uint8_t pairingKey[CRYPTO_KEY_SIZE];
};

static std::string headerOrBody(const Request& req, const char* header, const char* bodyKey) {
    const auto hdr = req.headers.find(header);
    if (hdr != req.headers.end() && !hdr->second.empty()) return hdr->second;
    if (!req.body.empty()) return jsonStr(parseBody(req.body), bodyKey);
    return "";
}

// The 401 every unauthenticated client call gets, counted by cause: a stranger
// is NOT_PAIRED, a paired device with the wrong key is BAD_PROOF, and the two
// never swap, so a stranger learns nothing about which ids are paired.
static void rejectUnauthorized(const Request& req, Response& res, const char* code,
                               const std::string& deviceId) {
    const bool badProof = std::string(code) == "BAD_PROOF";
    if (badProof) {
        satellite::g_wire.authBadProof.fetch_add(1, std::memory_order_relaxed);
    } else {
        satellite::g_wire.authNotPaired.fetch_add(1, std::memory_order_relaxed);
    }
    logMsg(LogLevel::WARN, "client",
           "401 unauthorized " + req.method + " " + req.path + " (" + code +
               (deviceId.empty() ? ", no deviceId supplied" : ", deviceId " + deviceId) + ")");
    res.status = 401;
    JsonOut err;
    err["error"] = "unauthorized";
    err["code"] = code;
    replyJson(res, jsonDump(err));
}

// Every authenticated client route requires a paired deviceId AND an hmacProof
// of the pairing key, so a diverged key fails HERE with a terminal 401 instead
// of a silently-undecryptable UDP session.
static bool clientAuthed(const Request& req, Response& res, ClientAuth& out) {
    out.deviceId = headerOrBody(req, "X-Device-Id", "deviceId");
    const std::string proof = headerOrBody(req, "X-Hmac-Proof", "hmacProof");

    const char* code = "NOT_PAIRED";
    if (!out.deviceId.empty() && findPairedDevice(out.deviceId, out.device)) {
        const bool proven = hexDecode(out.device.sharedKeyHex, out.pairingKey, CRYPTO_KEY_SIZE) &&
                            verifyHmacProof(out.pairingKey, out.deviceId, proof);
        if (proven) return true;
        code = "BAD_PROOF";
    }
    rejectUnauthorized(req, res, code, out.deviceId);
    return false;
}

// The satellite accepts [PROTOCOL_VERSION_MIN, PROTOCOL_VERSION] and settles the
// session on the client's offer; an absent field is a pre-versioning client and
// reads as 1. Only an out-of-range offer is refused, with `supported` (the newest
// this satellite speaks) telling the client which side must update.
static bool protocolVersionOk(const std::string& body, Response& res, long& pv) {
    pv = 1;
    jsonTryInt(parseBody(body), "protocolVersion", pv);
    if (pv >= PROTOCOL_VERSION_MIN && pv <= PROTOCOL_VERSION) return true;
    res.status = 409;
    JsonOut err;
    err["error"] = "protocol version unsupported";
    err["supported"] = PROTOCOL_VERSION;
    err["supportedMin"] = PROTOCOL_VERSION_MIN;
    replyJson(res, jsonDump(err));
    return false;
}

static JsonOut controllerApplyObj(const ControllerApplyResult& r) {
    JsonOut j;
    j["ctrlIdx"] = r.ctrlIdx;
    j["result"] = applyResultName(r.result);
    j["appliedType"] = r.appliedType;
    if (r.backendId.empty()) {
        j["backend"] = nullptr;
    } else {
        j["backend"] = r.backendId;
    }
    JsonOut motion;
    motion["sinkSupportedForType"] = r.motionSinkSupportedForType;
    motion["backendOk"] = r.motionBackendOk;
    j["motion"] = std::move(motion);
    return j;
}

static JsonOut mouseControlObj(bool granted, const std::string& denyReason) {
    JsonOut j;
    j["granted"] = granted;
    if (!granted && !denyReason.empty()) j["reason"] = denyReason;
    return j;
}

static std::string buildUpsertResponseJson(const SessionUpsertResult& r) {
    JsonOut j;
    j["connectionId"] = r.connectionId;
    j["token"] = hexEncodeBE32(r.token);
    j["sessionSalt"] = hexEncode(r.sessionSalt, SESSION_SALT_SIZE);
    j["epoch"] = r.epoch;
    j["maxControllers"] = r.maxControllers;
    j["protocolVersion"] = r.protocolVersion;
    JsonOut controllers = JsonOut::array();
    for (const auto& c : r.controllers) controllers.push_back(controllerApplyObj(c));
    j["controllers"] = std::move(controllers);
    JsonOut hostFeatures;
    hostFeatures["mouseControl"] = mouseControlObj(r.mouseControlGranted, r.mouseControlDenyReason);
    j["hostFeatures"] = std::move(hostFeatures);
    return jsonDump(j);
}

static JsonOut sessionViewControllerObj(const SessionService::SessionView::CtrlView& c) {
    JsonOut o;
    o["ctrlIdx"] = c.ctrlIdx;
    o["active"] = true;
    o["appliedType"] = c.appliedType;
    o["caps"] = capsJsonObj(c.caps);
    o["touchpadMode"] = touchpadModeName(c.touchpadMode);
    if (c.backendId.empty()) {
        o["backend"] = nullptr;
    } else {
        o["backend"] = c.backendId;
    }
    if (c.preferredBackend.empty()) {
        o["preferredBackend"] = nullptr;
    } else {
        o["preferredBackend"] = c.preferredBackend;
    }
    JsonOut motion;
    motion["sinkSupportedForType"] = c.motionSinkSupportedForType;
    motion["backendOk"] = c.motionBackendOk;
    o["motion"] = std::move(motion);
    return o;
}

static std::string buildSessionViewJson(const SessionService::SessionView& v) {
    JsonOut j;
    j["connectionId"] = v.connectionId;
    j["deviceId"] = v.deviceId;
    j["epoch"] = v.epoch;
    j["protocolVersion"] = v.protocolVersion;
    j["maxControllers"] = MAX_BACKEND_CONTROLLERS;
    JsonOut controllers = JsonOut::array();
    for (const auto& c : v.controllers) controllers.push_back(sessionViewControllerObj(c));
    j["controllers"] = std::move(controllers);
    JsonOut hostFeatures;
    hostFeatures["mouseControl"] = mouseControlObj(v.mouseControlGranted, "");
    j["hostFeatures"] = std::move(hostFeatures);
    return jsonDump(j);
}

// PUT /api/connections: the declarative upsert. Connect + full topology = ONE
// call; re-PUT converges; partial success rides in the body, never the status.
static void upsertConnectionRoute(SessionService& svc, const Request& req, Response& res) {
    if (!g_appRunning) {
        replyError(res, 503, "shutting down");
        return;
    }
    ClientAuth auth;
    if (!clientAuthed(req, res, auth)) return;
    // Read as {}, a garbled body would be the empty desired set and unplug every pad.
    Json body;
    if (!jsonRequestBody(req.body, body)) {
        replyError(res, 400, "body must be a JSON object");
        return;
    }
    long pv = PROTOCOL_VERSION;
    if (!protocolVersionOk(req.body, res, pv)) return;
    if (pv < PROTOCOL_VERSION) {
        logMsg(LogLevel::WARN, "client",
               "device " + auth.deviceId + " speaks protocol v" + std::to_string(pv) +
                   " (current v" + std::to_string(PROTOCOL_VERSION) +
                   "): update the Dish app for the full feature set");
    }

    std::string deviceName = jsonStr(body, "deviceName");
    if (deviceName.empty()) deviceName = auth.device.name;

    std::vector<ControllerDescriptor> descriptors;
    if (!parseControllerDescriptors(body, descriptors)) {
        logMsg(LogLevel::WARN, "client",
               "PUT /api/connections: malformed controllers array (ctrlIdx and type are "
               "required) from " +
                   auth.deviceId);
        replyError(res, 400, "controllers entries require ctrlIdx and type");
        return;
    }

    const bool mouseRequested = jsonBool(jsonObject(body, "hostFeatures"), "mouseControl");

    const auto result =
        svc.upsertSession(auth.deviceId, deviceName, req.remote_addr, auth.pairingKey, descriptors,
                          mouseRequested, static_cast<int>(pv));
    if (!result.ok) {
        replyError(res, 500, result.error);
        return;
    }
    refreshPairedDeviceIdentity(auth.deviceId, req.remote_addr, deviceName);
    replyJson(res, buildUpsertResponseJson(result));
}

// What one POST /api/pair carries. Read once; each path below takes what it
// needs.
struct PairRequest {
    std::string deviceId;
    std::string deviceName;
    std::string pin;         // server-shown PIN (Path A)
    std::string clientPin;   // dish-shown PIN (Path B)
    std::string clientPkHex; // client's X25519 public key
    std::string hmacProof;   // key-rotation proof
    std::string clientIP;
    long protocolVersion = PROTOCOL_VERSION;
};

static PairRequest readPairRequest(const Request& req) {
    const Json body = parseBody(req.body);
    PairRequest pr;
    pr.deviceId = jsonStr(body, "deviceId");
    pr.deviceName = jsonStr(body, "deviceName");
    pr.pin = jsonStr(body, "pin");
    pr.clientPin = jsonStr(body, "clientPin");
    pr.clientPkHex = jsonStr(body, "publicKey");
    pr.hmacProof = jsonStr(body, "hmacProof");
    pr.clientIP = req.remote_addr;
    return pr;
}

// Key rotation / re-pair with proof of the current key. False when the proof
// does not hold (or the record vanished under it), in which case the caller
// falls through to the PIN paths, identical to a fresh pairing attempt.
static bool tryRotateKey(SessionService& svc, const PairRequest& pr, Response& res) {
    PairedDevice dev;
    uint8_t currentKey[CRYPTO_KEY_SIZE];
    const bool proven = findPairedDevice(pr.deviceId, dev) &&
                        hexDecode(dev.sharedKeyHex, currentKey, CRYPTO_KEY_SIZE) &&
                        verifyHmacProof(currentKey, pr.deviceId, pr.hmacProof);
    std::string newKeyHex;
    const bool rotated = proven && rotatePairedDeviceKey(pr.deviceId, pr.clientIP, newKeyHex);
    if (!rotated) {
        logMsg(LogLevel::WARN, "pairing",
               "Rejected proof-based re-pair for " + pr.deviceId + " (" + pr.clientIP + ")");
        return false;
    }
    // The old key dies with the rotation, so any live session keyed on it
    // must die too.
    svc.closeSessionsForDevice(pr.deviceId, CLOSE_REASON_REPLACED);
    logMsg(LogLevel::INFO, "pairing",
           "Rotated pairing key for " + pr.deviceId + " (" + pr.clientIP + ")");
    JsonOut ok;
    ok["ok"] = true;
    ok["message"] = "key rotated";
    ok["sharedKey"] = newKeyHex;
    ok["protocolVersion"] = pr.protocolVersion;
    replyJson(res, jsonDump(ok));
    return true;
}

// Path A: the dish entered the operator's server-generated PIN. True when it
// answered, either way; false on a wrong PIN, which falls through.
static bool pairWithServerPin(SessionService& svc, const PairRequest& pr, Response& res) {
    uint8_t serverPk[32];
    uint8_t serverSk[32];
    generateKeyPair(serverPk, serverSk);

    // Key resolved BEFORE the PIN: a successful verifyPin consumes and rotates
    // the operator PIN, which a malformed key must not burn.
    std::string sharedKeyHex;
    const PairingKeyOutcome outcome =
        resolvePairingSharedKey(pr.clientPkHex, serverPk, serverSk, sharedKeyHex);
    if (outcome == PairingKeyOutcome::InvalidClientKey) {
        sodium_memzero(serverSk, 32);
        logMsg(LogLevel::WARN, "pairing",
               "Rejected pairing: unusable client public key from " + pr.deviceId + " (" +
                   pr.clientIP + ")");
        replyJson(res, R"({"ok":false,"error":"invalid public key"})");
        return true;
    }
    if (!verifyPin(pr.pin)) {
        sodium_memzero(serverSk, 32);
        return false;
    }

    upsertPairedDevice(pr.deviceId, pr.deviceName, pr.clientIP, sharedKeyHex);
    // A re-pair invalidates the previous key; a session still keyed on it
    // would churn undecryptably, so close it now.
    svc.closeSessionsForDevice(pr.deviceId, CLOSE_REASON_REPLACED);

    const std::string serverPkHex = hexEncode(serverPk, 32);
    sodium_memzero(serverSk, 32);
    logMsg(LogLevel::INFO, "pairing",
           "Paired device via server PIN: " + pr.deviceId + " (" + pr.clientIP + ")");
    JsonOut ok;
    ok["ok"] = true;
    ok["message"] = "paired successfully";
    if (outcome == PairingKeyOutcome::Derived) {
        ok["serverPublicKey"] = serverPkHex;
    } else {
        ok["sharedKey"] = sharedKeyHex;
    }
    ok["protocolVersion"] = pr.protocolVersion;
    replyJson(res, jsonDump(ok));
    return true;
}

// Path B: register the dish's request; it then polls /api/pair/status. The
// clientPin is never echoed server-side; the operator must read it off the
// dish, which is what makes the accept meaningful.
static void registerClientPinRequest(const PairRequest& pr, Response& res) {
    submitPairRequest(pr.deviceId, pr.deviceName, pr.clientIP, pr.clientPin);
    logMsg(LogLevel::INFO, "pairing",
           "Pairing request from " + (pr.deviceName.empty() ? pr.deviceId : pr.deviceName) + " (" +
               pr.clientIP + ") awaiting operator approval");
    replyJson(res, R"({"ok":false,"pending":true,"message":"awaiting approval on the satellite"})");
}

// Dual-path device pairing over HTTPS.
// Path A: `pin` (server-generated, typed into the dish), verifyPin, pair now.
// Path B: `clientPin` (dish-shown), register a request, reply pending=true; the
//   operator accepts on the dashboard/tray and the dish polls /api/pair/status.
// Key rotation: `hmacProof` of the CURRENT key mints and returns a fresh key.
// There is NO PIN-free already-paired short-circuit: handing the stored key to
// anyone who learned a deviceId would let any LAN actor exfiltrate it.
// Always 200 on the PIN paths; the sender classifies on `ok`/`pending`.
static void pairRoute(SessionService& svc, const Request& req, Response& res) {
    PairRequest pr = readPairRequest(req);
    if (pr.deviceId.empty()) {
        replyJson(res, R"({"ok":false,"error":"missing deviceId"})");
        return;
    }
    if (!protocolVersionOk(req.body, res, pr.protocolVersion)) return;
    if (!pr.hmacProof.empty() && tryRotateKey(svc, pr, res)) return;
    if (!pr.pin.empty() && pairWithServerPin(svc, pr, res)) return;
    if (!pr.clientPin.empty()) {
        registerClientPinRequest(pr, res);
        return;
    }
    logMsg(LogLevel::WARN, "pairing", "Invalid or empty PIN attempt from " + pr.clientIP);
    replyJson(res, R"({"ok":false,"error":"invalid or expired PIN"})");
}

// Path-B poll. No device auth (not paired yet); the minted key is handed back
// exactly once on approval (pollPairRequest clears it).
static void pairStatusRoute(const Request& req, Response& res) {
    const std::string deviceId = req.has_param("deviceId") ? req.get_param_value("deviceId") : "";
    if (deviceId.empty()) {
        res.status = 400;
        replyJson(res, R"({"ok":false,"error":"missing deviceId"})");
        return;
    }
    std::string keyHex;
    const PairRequestState st = pollPairRequest(deviceId, keyHex);
    if (st == PairRequestState::Approved) {
        JsonOut ok;
        ok["ok"] = true;
        ok["status"] = "approved";
        ok["sharedKey"] = keyHex;
        replyJson(res, jsonDump(ok));
        return;
    }
    JsonOut r;
    r["ok"] = false;
    r["status"] = pairRequestStateName(st);
    replyJson(res, jsonDump(r));
}

// DELETE /api/pair: client self-unpair (hmacProof-authed). Closes any live
// session first (close-notify reason=unpaired rides the still-valid key).
static void selfUnpairRoute(SessionService& svc, const Request& req, Response& res) {
    ClientAuth auth;
    if (!clientAuthed(req, res, auth)) return;

    svc.closeSessionsForDevice(auth.deviceId, CLOSE_REASON_UNPAIRED);
    const bool wasPaired = forgetPairedDevice(auth.deviceId);
    logMsg(LogLevel::INFO, "pairing",
           "Device self-unpaired: " + auth.deviceId +
               (wasPaired ? "" : " (record already gone: an admin unpair raced it)"));
    replyOk(res);
}

// GET /api/connections/:id: the reconcile endpoint, scoped to OWN session.
static void sessionViewRoute(SessionService& svc, const Request& req, Response& res) {
    ClientAuth auth;
    if (!clientAuthed(req, res, auth)) return;
    const auto view = svc.getSessionView(req.matches[1].str(), auth.deviceId);
    if (!view.found) {
        replyError(res, 404, "connection not found");
        return;
    }
    replyJson(res, buildSessionViewJson(view));
}

// DELETE /api/connections/:id: graceful close of OWN session (no notify: the
// closer already knows).
static void closeSessionRoute(SessionService& svc, const Request& req, Response& res) {
    ClientAuth auth;
    if (!clientAuthed(req, res, auth)) return;
    const int removed = svc.closeSessionById(req.matches[1].str(), auth.deviceId,
                                             CLOSE_REASON_REPLACED, /*notify=*/false);
    if (removed < 0) {
        replyError(res, 404, "connection not found");
        return;
    }
    JsonOut ok;
    ok["ok"] = true;
    ok["controllersRemoved"] = removed;
    replyJson(res, jsonDump(ok));
}

// The controller index from a /controllers/:idx path, saturated to the byte
// the wire carries.
static uint8_t ctrlIdxFromPath(const Request& req) {
    return saturateToByte(strtol(req.matches[2].str().c_str(), nullptr, 10));
}

// PUT /api/connections/:id/controllers/:idx: standalone single-descriptor
// upsert (the FULL descriptor; ctrlIdx in the path wins). No version gate here:
// the version is negotiated once, on the session PUT, and a sub-resource write
// inherits its session's settled version.
static void putControllerRoute(SessionService& svc, const Request& req, Response& res) {
    ClientAuth auth;
    if (!clientAuthed(req, res, auth)) return;
    ControllerDescriptor d;
    if (!parseDescriptorObject(parseBody(req.body), /*requireIdx=*/false, d)) {
        replyError(res, 400, "descriptor requires type");
        return;
    }
    d.ctrlIdx = ctrlIdxFromPath(req);
    ControllerApplyResult ar;
    uint16_t epoch = 0;
    if (!svc.applyController(req.matches[1].str(), auth.deviceId, d, ar, epoch)) {
        replyError(res, 404, "connection not found");
        return;
    }
    JsonOut j;
    j["epoch"] = epoch;
    j["controller"] = controllerApplyObj(ar);
    replyJson(res, jsonDump(j));
}

// DELETE /api/connections/:id/controllers/:idx: removes the SLOT only; the
// session lives on (zero-controller sessions are valid).
static void deleteControllerRoute(SessionService& svc, const Request& req, Response& res) {
    ClientAuth auth;
    if (!clientAuthed(req, res, auth)) return;
    uint16_t epoch = 0;
    if (!svc.removeController(req.matches[1].str(), auth.deviceId, ctrlIdxFromPath(req), epoch)) {
        replyError(res, 404, "connection not found");
        return;
    }
    JsonOut ok;
    ok["ok"] = true;
    ok["epoch"] = epoch;
    replyJson(res, jsonDump(ok));
}

static void capabilitiesRoute(const Request&, Response& res) {
    replyJson(res, buildCapabilitiesJson());
}

// Catalog routes are unauthenticated: the UI renders BEFORE pairing.
static void catalogRoute(const Request& req, Response& res) {
    const std::string locale =
        satellite::resolveCatalogLocale(req.get_header_value("Accept-Language"));
    const std::string etag = satellite::catalogETag(SATELLITE_VERSION, locale);
    res.set_header("ETag", etag);
    res.set_header("Vary", "Accept-Language");
    if (req.get_header_value("If-None-Match") == etag) {
        res.status = 304;
        return;
    }
    const std::string langJson = readFile(g_webDir + "/lang/" + locale + ".json");
    const std::string enJson = (locale == "en") ? langJson : readFile(g_webDir + "/lang/en.json");
    replyJson(res, satellite::buildCatalogJson(locale, langJson, enJson, SATELLITE_VERSION,
                                               catalogBackendTraits()));
}

static void catalogImageRoute(const Request& req, Response& res) {
    const std::string slug = req.matches[1].str();
    const auto slugs = satellite::catalogImageSlugs();
    const bool known = std::find(slugs.begin(), slugs.end(), slug) != slugs.end();
    const std::string svg = known ? readFile(g_webDir + "/img/catalog/" + slug + ".svg") : "";
    if (svg.empty()) {
        replyError(res, 404, "unknown catalog image");
        return;
    }
    const std::string etag = std::string("\"") + SATELLITE_VERSION + "\"";
    res.set_header("ETag", etag);
    if (req.get_header_value("If-None-Match") == etag) {
        res.status = 304;
        return;
    }
    res.set_content(svg, "image/svg+xml");
}

// Client API server routes: pairing + sessions + catalog.
void registerClientRoutes(httplib::Server& server, SessionService& svc) {
    // POST /api/pair: PIN-gated (or hmacProof-gated rotation); no device auth
    // for the PIN paths (the device is not paired yet).
    server.Post("/api/pair",
                [&svc](const Request& req, Response& res) { pairRoute(svc, req, res); });
    server.Get("/api/pair/status", pairStatusRoute);
    server.Delete("/api/pair",
                  [&svc](const Request& req, Response& res) { selfUnpairRoute(svc, req, res); });

    // PUT /api/connections: idempotent session upsert keyed on deviceId.
    server.Put("/api/connections",
               [&svc](const Request& req, Response& res) { upsertConnectionRoute(svc, req, res); });
    server.Get(R"(/api/connections/(\w+))",
               [&svc](const Request& req, Response& res) { sessionViewRoute(svc, req, res); });
    server.Delete(R"(/api/connections/(\w+))",
                  [&svc](const Request& req, Response& res) { closeSessionRoute(svc, req, res); });
    server.Put(R"(/api/connections/(\w+)/controllers/(\d+))",
               [&svc](const Request& req, Response& res) { putControllerRoute(svc, req, res); });
    server.Delete(
        R"(/api/connections/(\w+)/controllers/(\d+))",
        [&svc](const Request& req, Response& res) { deleteControllerRoute(svc, req, res); });

    // No auth on the read-only info surface: the client UI renders BEFORE pairing.
    server.Get("/api/server/capabilities", capabilitiesRoute);
    server.Get("/api/catalog", catalogRoute);
    server.Get(R"(/api/catalog/images/([\w-]+))", catalogImageRoute);
}
