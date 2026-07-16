// SPDX-License-Identifier: LGPL-3.0-or-later

// Client API (sender-facing HTTPS) route handlers, moved verbatim from
// webserver.cpp in the D10 decomposition. Handlers and builders used only by
// this surface stay file-static; the route registrations were already written
// against a local `server` reference, so no line inside the moved code
// changed.
#include "routes_client.h"
#include "routes_common.h"
#include "crypto.h"
#include "config.h"
#include "pairing.h"
#include "pairing_keys.h"
#include "pairing_service.h"
#include "session_crypto.h"
#include "core/catalog.h"
#include "core/json.h"
#include "core/session_service.h"
#include "core/version.h"

#include <sodium.h>

using satellite::Json;
using satellite::jsonBool;
using satellite::jsonDump;
using satellite::jsonObject;
using satellite::JsonOut;
using satellite::jsonStr;
using satellite::jsonTryInt;

static JsonOut capsJsonObj(uint16_t caps) {
    JsonOut j;
    j["rumble"] = (caps & CAP_RUMBLE) != 0;
    j["motion"] = (caps & CAP_MOTION) != 0;
    j["analogTriggers"] = (caps & CAP_ANALOG_TRIGGERS) != 0;
    j["lightbar"] = (caps & CAP_LIGHTBAR) != 0;
    return j;
}

struct ClientAuth {
    std::string deviceId;
    PairedDevice device; // copy-by-value under g_configMtx, never a pointer
    uint8_t pairingKey[CRYPTO_KEY_SIZE];
};

static std::string headerOrBody(const httplib::Request& req, const char* header,
                                const char* bodyKey) {
    auto hdr = req.headers.find(header);
    if (hdr != req.headers.end() && !hdr->second.empty()) return hdr->second;
    if (!req.body.empty()) return jsonStr(parseBody(req.body), bodyKey);
    return "";
}

// Every authenticated client route requires a paired deviceId AND an hmacProof
// of the pairing key, so a diverged key fails HERE with a terminal 401 instead
// of a silently-undecryptable UDP session. The PairedDevice is copied by value
// under g_configMtx so a concurrent unpair can't dangle it.
static bool clientAuthed(const httplib::Request& req, httplib::Response& res, ClientAuth& out) {
    out.deviceId = headerOrBody(req, "X-Device-Id", "deviceId");
    const std::string proof = headerOrBody(req, "X-Hmac-Proof", "hmacProof");

    const char* code = "NOT_PAIRED";
    if (!out.deviceId.empty()) {
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_configMtx);
            for (const auto& d : g_config.pairedDevices) {
                if (d.id == out.deviceId) {
                    out.device = d;
                    found = true;
                    break;
                }
            }
        }
        if (found) {
            if (hexDecode(out.device.sharedKeyHex, out.pairingKey, CRYPTO_KEY_SIZE) &&
                verifyHmacProof(out.pairingKey, out.deviceId, proof)) {
                return true;
            }
            code = "BAD_PROOF";
        }
    }

    logMsg(LogLevel::WARN, "client",
           "401 unauthorized " + req.method + " " + req.path + " (" + code +
               (out.deviceId.empty() ? ", no deviceId supplied" : ", deviceId " + out.deviceId) +
               ")");
    res.status = 401;
    JsonOut err;
    err["error"] = "unauthorized";
    err["code"] = code;
    res.set_content(jsonDump(err), "application/json");
    return false;
}

static bool protocolVersionOk(const std::string& body, httplib::Response& res) {
    long pv = PROTOCOL_VERSION;
    if (jsonTryInt(parseBody(body), "protocolVersion", pv) && pv != PROTOCOL_VERSION) {
        res.status = 409;
        JsonOut err;
        err["error"] = "protocol version unsupported";
        err["supported"] = PROTOCOL_VERSION;
        res.set_content(jsonDump(err), "application/json");
        return false;
    }
    return true;
}

// `type` is REQUIRED: a descriptor without it would force a server-side default
// type, the default-then-correct bug class this contract removes.
static bool parseDescriptorObject(const Json& obj, bool requireIdx, ControllerDescriptor& d) {
    long idx = 0;
    if (jsonTryInt(obj, "ctrlIdx", idx)) {
        if (idx < 0) return false;
        d.ctrlIdx = idx > 255 ? 255 : static_cast<uint8_t>(idx);
    } else if (requireIdx) {
        return false;
    }
    long type = 0;
    if (!jsonTryInt(obj, "type", type) || type < 0) return false;
    // Out-of-range values pass through; the service reports invalidType per
    // controller rather than failing the whole request.
    d.type = type > 255 ? 255 : static_cast<uint8_t>(type);

    d.caps = 0;
    const Json caps = jsonObject(obj, "caps");
    if (jsonBool(caps, "rumble")) d.caps |= CAP_RUMBLE;
    if (jsonBool(caps, "motion")) d.caps |= CAP_MOTION;
    if (jsonBool(caps, "analogTriggers")) d.caps |= CAP_ANALOG_TRIGGERS;
    if (jsonBool(caps, "lightbar")) d.caps |= CAP_LIGHTBAR;

    const std::string mode = jsonStr(obj, "touchpadMode");
    if (mode == "ds4") {
        d.touchpadMode = TOUCHPAD_MODE_DS4;
    } else if (mode == "mouse") {
        d.touchpadMode = TOUCHPAD_MODE_MOUSE;
    } else {
        d.touchpadMode = TOUCHPAD_MODE_OFF;
    }
    return true;
}

static bool parseControllerDescriptors(const Json& body, std::vector<ControllerDescriptor>& out) {
    auto it = body.find("controllers");
    if (it == body.end() || !it->is_array()) return true; // absent → no descriptors
    for (const auto& obj : *it) {
        if (!obj.is_object()) continue; // ignore non-object array entries, as before
        ControllerDescriptor d;
        if (!parseDescriptorObject(obj, /*requireIdx=*/true, d)) return false;
        out.push_back(d);
    }
    return true;
}

static JsonOut controllerApplyObj(const ControllerApplyResult& r) {
    JsonOut j;
    j["ctrlIdx"] = r.ctrlIdx;
    j["result"] = applyResultName(r.result);
    j["appliedType"] = r.appliedType;
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
    char tokenHex[9];
    snprintf(tokenHex, sizeof(tokenHex), "%08x", r.token);

    JsonOut j;
    j["connectionId"] = r.connectionId;
    j["token"] = std::string(tokenHex);
    j["sessionSalt"] = hexEncode(r.sessionSalt, SESSION_SALT_SIZE);
    j["epoch"] = r.epoch;
    j["maxControllers"] = r.maxControllers;
    j["protocolVersion"] = PROTOCOL_VERSION;
    JsonOut controllers = JsonOut::array();
    for (const auto& c : r.controllers) controllers.push_back(controllerApplyObj(c));
    j["controllers"] = std::move(controllers);
    JsonOut hostFeatures;
    hostFeatures["mouseControl"] = mouseControlObj(r.mouseControlGranted, r.mouseControlDenyReason);
    j["hostFeatures"] = std::move(hostFeatures);
    return jsonDump(j);
}

static std::string buildSessionViewJson(const SessionService::SessionView& v) {
    JsonOut j;
    j["connectionId"] = v.connectionId;
    j["deviceId"] = v.deviceId;
    j["epoch"] = v.epoch;
    j["protocolVersion"] = PROTOCOL_VERSION;
    j["maxControllers"] = MAX_BACKEND_CONTROLLERS;
    JsonOut controllers = JsonOut::array();
    for (const auto& c : v.controllers) {
        JsonOut o;
        o["ctrlIdx"] = c.ctrlIdx;
        o["active"] = true;
        o["appliedType"] = c.appliedType;
        o["caps"] = capsJsonObj(c.caps);
        o["touchpadMode"] = touchpadModeName(c.touchpadMode);
        JsonOut motion;
        motion["sinkSupportedForType"] = c.motionSinkSupportedForType;
        motion["backendOk"] = c.motionBackendOk;
        o["motion"] = std::move(motion);
        controllers.push_back(std::move(o));
    }
    j["controllers"] = std::move(controllers);
    JsonOut hostFeatures;
    hostFeatures["mouseControl"] = mouseControlObj(v.mouseControlGranted, "");
    j["hostFeatures"] = std::move(hostFeatures);
    return jsonDump(j);
}

// PUT /api/connections: the declarative upsert. Connect + full topology = ONE
// call; re-PUT converges; partial success rides in the body, never the status.
static void upsertConnectionRoute(SessionService& svc, const httplib::Request& req,
                                  httplib::Response& res) {
    if (!g_appRunning) {
        res.status = 503;
        res.set_content(R"({"error":"shutting down"})", "application/json");
        return;
    }
    ClientAuth auth;
    if (!clientAuthed(req, res, auth)) return;
    if (!protocolVersionOk(req.body, res)) return;

    Json body = parseBody(req.body);
    std::string deviceName = jsonStr(body, "deviceName");
    if (deviceName.empty()) deviceName = auth.device.name;

    std::vector<ControllerDescriptor> descriptors;
    if (!parseControllerDescriptors(body, descriptors)) {
        logMsg(LogLevel::WARN, "client",
               "PUT /api/connections: malformed controllers array (ctrlIdx and type are "
               "required) from " +
                   auth.deviceId);
        res.status = 400;
        res.set_content(R"({"error":"controllers entries require ctrlIdx and type"})",
                        "application/json");
        return;
    }

    bool mouseRequested = jsonBool(jsonObject(body, "hostFeatures"), "mouseControl");

    auto result = svc.upsertSession(auth.deviceId, deviceName, req.remote_addr, auth.pairingKey,
                                    descriptors, mouseRequested);
    if (!result.ok) {
        res.status = 500;
        JsonOut err;
        err["error"] = result.error;
        res.set_content(jsonDump(err), "application/json");
        return;
    }

    // Refresh the paired record's last-seen identity (name can change on the
    // client between sessions).
    {
        std::lock_guard<std::mutex> lk(g_configMtx);
        for (auto& d : g_config.pairedDevices) {
            if (d.id == auth.deviceId) {
                d.lastIP = req.remote_addr;
                d.name = deviceName;
                break;
            }
        }
        saveConfig(g_config);
    }

    res.set_content(buildUpsertResponseJson(result), "application/json");
}

// Dual-path device pairing over HTTPS.
// Path A: `pin` (server-generated, typed into the dish), verifyPin, pair now.
// Path B: `clientPin` (dish-shown), register a request, reply pending=true; the
//   operator accepts on the dashboard/tray and the dish polls /api/pair/status.
// Key rotation: `hmacProof` of the CURRENT key mints and returns a fresh key.
// There is NO PIN-free already-paired short-circuit: handing the stored key to
// anyone who learned a deviceId would let any LAN actor exfiltrate it.
// Always 200 on the PIN paths; the sender classifies on `ok`/`pending`.
static void pairRoute(SessionService& svc, const httplib::Request& req, httplib::Response& res) {
    Json body = parseBody(req.body);
    auto deviceId = jsonStr(body, "deviceId");
    auto deviceName = jsonStr(body, "deviceName");
    auto pin = jsonStr(body, "pin");               // server-shown PIN (Path A)
    auto clientPin = jsonStr(body, "clientPin");   // dish-shown PIN (Path B)
    auto clientPkHex = jsonStr(body, "publicKey"); // client's X25519 public key
    auto hmacProof = jsonStr(body, "hmacProof");   // key-rotation proof
    const std::string clientIP = req.remote_addr;

    if (deviceId.empty()) {
        res.set_content(R"({"ok":false,"error":"missing deviceId"})", "application/json");
        return;
    }
    if (!protocolVersionOk(req.body, res)) return;

    // Key rotation / re-pair with proof of the current key. A failed proof
    // falls through to the PIN paths, identical to a fresh pairing attempt.
    if (!hmacProof.empty()) {
        PairedDevice dev;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_configMtx);
            for (const auto& d : g_config.pairedDevices) {
                if (d.id == deviceId) {
                    dev = d;
                    found = true;
                    break;
                }
            }
        }
        uint8_t currentKey[CRYPTO_KEY_SIZE];
        if (found && hexDecode(dev.sharedKeyHex, currentKey, CRYPTO_KEY_SIZE) &&
            verifyHmacProof(currentKey, deviceId, hmacProof)) {
            std::string newKeyHex;
            rotatePairedDeviceKey(deviceId, clientIP, newKeyHex);
            // The old key dies with the rotation, so any live session keyed on
            // it must die too.
            svc.closeSessionsForDevice(deviceId, CLOSE_REASON_REPLACED);
            logMsg(LogLevel::INFO, "pairing",
                   "Rotated pairing key for " + deviceId + " (" + clientIP + ")");
            JsonOut ok;
            ok["ok"] = true;
            ok["message"] = "key rotated";
            ok["sharedKey"] = newKeyHex;
            ok["protocolVersion"] = PROTOCOL_VERSION;
            res.set_content(jsonDump(ok), "application/json");
            return;
        }
        logMsg(LogLevel::WARN, "pairing",
               "Rejected proof-based re-pair for " + deviceId + " (" + clientIP + ")");
    }

    // Path A: dish entered the operator's server-generated PIN.
    if (!pin.empty() && verifyPin(pin)) {
        uint8_t serverPk[32], serverSk[32];
        generateKeyPair(serverPk, serverSk);

        std::string sharedKeyHex;
        PairingKeyOutcome outcome =
            resolvePairingSharedKey(clientPkHex, serverPk, serverSk, sharedKeyHex);
        if (outcome == PairingKeyOutcome::InvalidClientKey) {
            sodium_memzero(serverSk, 32);
            logMsg(LogLevel::WARN, "pairing",
                   "Rejected pairing: unusable client public key from " + deviceId + " (" +
                       clientIP + ")");
            res.set_content(R"({"ok":false,"error":"invalid public key"})", "application/json");
            return;
        }

        upsertPairedDevice(deviceId, deviceName, clientIP, sharedKeyHex);
        // A re-pair invalidates the previous key; a session still keyed on it
        // would churn undecryptably, so close it now.
        svc.closeSessionsForDevice(deviceId, CLOSE_REASON_REPLACED);

        std::string serverPkHex = hexEncode(serverPk, 32);
        sodium_memzero(serverSk, 32);
        logMsg(LogLevel::INFO, "pairing",
               "Paired device via server PIN: " + deviceId + " (" + clientIP + ")");
        JsonOut ok;
        ok["ok"] = true;
        ok["message"] = "paired successfully";
        if (outcome == PairingKeyOutcome::Derived) {
            ok["serverPublicKey"] = serverPkHex;
        } else {
            ok["sharedKey"] = sharedKeyHex;
        }
        ok["protocolVersion"] = PROTOCOL_VERSION;
        res.set_content(jsonDump(ok), "application/json");
        return;
    }

    // Path B: register the dish's request; it then polls /api/pair/status. The
    // clientPin is never echoed server-side; the operator must read it off the
    // dish, which is what makes the accept meaningful.
    if (!clientPin.empty()) {
        submitPairRequest(deviceId, deviceName, clientIP, clientPin);
        logMsg(LogLevel::INFO, "pairing",
               "Pairing request from " + (deviceName.empty() ? deviceId : deviceName) + " (" +
                   clientIP + ") awaiting operator approval");
        res.set_content(
            R"({"ok":false,"pending":true,"message":"awaiting approval on the satellite"})",
            "application/json");
        return;
    }

    logMsg(LogLevel::WARN, "pairing", "Invalid or empty PIN attempt from " + clientIP);
    res.set_content(R"({"ok":false,"error":"invalid or expired PIN"})", "application/json");
}

// DELETE /api/pair: client self-unpair (hmacProof-authed). Closes any live
// session first (close-notify reason=unpaired rides the still-valid key).
static void selfUnpairRoute(SessionService& svc, const httplib::Request& req,
                            httplib::Response& res) {
    ClientAuth auth;
    if (!clientAuthed(req, res, auth)) return;

    svc.closeSessionsForDevice(auth.deviceId, CLOSE_REASON_UNPAIRED);
    {
        std::lock_guard<std::mutex> lk(g_configMtx);
        auto& devs = g_config.pairedDevices;
        devs.erase(std::remove_if(devs.begin(), devs.end(),
                                  [&](const PairedDevice& d) { return d.id == auth.deviceId; }),
                   devs.end());
        saveConfig(g_config);
    }
    logMsg(LogLevel::INFO, "pairing", "Device self-unpaired: " + auth.deviceId);
    res.set_content(R"({"ok":true})", "application/json");
}

// Catalog routes are unauthenticated: the UI renders BEFORE pairing.
static void catalogRoute(const httplib::Request& req, httplib::Response& res) {
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
    res.set_content(satellite::buildCatalogJson(locale, langJson, enJson, SATELLITE_VERSION,
                                                catalogBackendTraits()),
                    "application/json");
}

static void catalogImageRoute(const httplib::Request& req, httplib::Response& res) {
    const std::string slug = req.matches[1].str();
    bool known = false;
    for (const auto& s : satellite::catalogImageSlugs()) {
        if (s == slug) {
            known = true;
            break;
        }
    }
    std::string svg = known ? readFile(g_webDir + "/img/catalog/" + slug + ".svg") : "";
    if (svg.empty()) {
        res.status = 404;
        res.set_content(R"({"error":"unknown catalog image"})", "application/json");
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
    server.Post("/api/pair", [&svc](const httplib::Request& req, httplib::Response& res) {
        pairRoute(svc, req, res);
    });

    // Path-B poll. No device auth (not paired yet); the minted key is handed
    // back exactly once on approval (pollPairRequest clears it).
    server.Get("/api/pair/status", [](const httplib::Request& req, httplib::Response& res) {
        std::string deviceId;
        if (req.has_param("deviceId")) deviceId = req.get_param_value("deviceId");
        if (deviceId.empty()) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"missing deviceId"})", "application/json");
            return;
        }
        std::string keyHex;
        PairRequestState st = pollPairRequest(deviceId, keyHex);
        if (st == PairRequestState::Approved) {
            JsonOut ok;
            ok["ok"] = true;
            ok["status"] = "approved";
            ok["sharedKey"] = keyHex;
            res.set_content(jsonDump(ok), "application/json");
            return;
        }
        JsonOut r;
        r["ok"] = false;
        r["status"] = pairRequestStateName(st);
        res.set_content(jsonDump(r), "application/json");
    });

    // DELETE /api/pair: client self-unpair (closes any live session first).
    server.Delete("/api/pair", [&svc](const httplib::Request& req, httplib::Response& res) {
        selfUnpairRoute(svc, req, res);
    });

    // PUT /api/connections: idempotent session upsert keyed on deviceId.
    server.Put("/api/connections", [&svc](const httplib::Request& req, httplib::Response& res) {
        upsertConnectionRoute(svc, req, res);
    });

    // GET /api/connections/:id: the reconcile endpoint, scoped to OWN session.
    server.Get(R"(/api/connections/(\w+))",
               [&svc](const httplib::Request& req, httplib::Response& res) {
                   ClientAuth auth;
                   if (!clientAuthed(req, res, auth)) return;
                   auto view = svc.getSessionView(req.matches[1].str(), auth.deviceId);
                   if (!view.found) {
                       res.status = 404;
                       res.set_content(R"({"error":"connection not found"})", "application/json");
                       return;
                   }
                   res.set_content(buildSessionViewJson(view), "application/json");
               });

    // DELETE /api/connections/:id: graceful close of OWN session (no notify:
    // the closer already knows).
    server.Delete(
        R"(/api/connections/(\w+))", [&svc](const httplib::Request& req, httplib::Response& res) {
            ClientAuth auth;
            if (!clientAuthed(req, res, auth)) return;
            int removed = svc.closeSessionById(req.matches[1].str(), auth.deviceId,
                                               CLOSE_REASON_REPLACED, /*notify=*/false);
            if (removed < 0) {
                res.status = 404;
                res.set_content(R"({"error":"connection not found"})", "application/json");
                return;
            }
            JsonOut ok;
            ok["ok"] = true;
            ok["controllersRemoved"] = removed;
            res.set_content(jsonDump(ok), "application/json");
        });

    // PUT /api/connections/:id/controllers/:idx: standalone single-descriptor
    // upsert (the FULL descriptor; ctrlIdx in the path wins).
    server.Put(R"(/api/connections/(\w+)/controllers/(\d+))", [&svc](const httplib::Request& req,
                                                                     httplib::Response& res) {
        ClientAuth auth;
        if (!clientAuthed(req, res, auth)) return;
        if (!protocolVersionOk(req.body, res)) return;
        ControllerDescriptor d;
        if (!parseDescriptorObject(parseBody(req.body), /*requireIdx=*/false, d)) {
            res.status = 400;
            res.set_content(R"({"error":"descriptor requires type"})", "application/json");
            return;
        }
        long idx = strtol(req.matches[2].str().c_str(), nullptr, 10);
        d.ctrlIdx = idx > 255 ? 255 : static_cast<uint8_t>(idx);
        ControllerApplyResult ar;
        uint16_t epoch = 0;
        if (!svc.applyController(req.matches[1].str(), auth.deviceId, d, ar, epoch)) {
            res.status = 404;
            res.set_content(R"({"error":"connection not found"})", "application/json");
            return;
        }
        JsonOut j;
        j["epoch"] = epoch;
        j["controller"] = controllerApplyObj(ar);
        res.set_content(jsonDump(j), "application/json");
    });

    // DELETE /api/connections/:id/controllers/:idx: removes the SLOT only;
    // the session lives on (zero-controller sessions are valid).
    server.Delete(R"(/api/connections/(\w+)/controllers/(\d+))", [&svc](const httplib::Request& req,
                                                                        httplib::Response& res) {
        ClientAuth auth;
        if (!clientAuthed(req, res, auth)) return;
        long idx = strtol(req.matches[2].str().c_str(), nullptr, 10);
        uint16_t epoch = 0;
        if (!svc.removeController(req.matches[1].str(), auth.deviceId,
                                  idx > 255 ? 255 : static_cast<uint8_t>(idx), epoch)) {
            res.status = 404;
            res.set_content(R"({"error":"connection not found"})", "application/json");
            return;
        }
        JsonOut ok;
        ok["ok"] = true;
        ok["epoch"] = epoch;
        res.set_content(jsonDump(ok), "application/json");
    });

    // No auth on the read-only info surface: the client UI renders BEFORE pairing.
    server.Get("/api/server/capabilities", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildCapabilitiesJson(), "application/json");
    });
    server.Get("/api/catalog",
               [](const httplib::Request& req, httplib::Response& res) { catalogRoute(req, res); });
    server.Get(
        R"(/api/catalog/images/([\w-]+))",
        [](const httplib::Request& req, httplib::Response& res) { catalogImageRoute(req, res); });
}
