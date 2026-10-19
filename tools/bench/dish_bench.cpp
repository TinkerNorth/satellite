// SPDX-License-Identifier: LGPL-3.0-or-later
//
// dish_bench - PC-side latency/throughput benchmark for the satellite receive path.
//
// What it does
// ------------
// Pretends to be a paired "dish" client: opens an encrypted protocol-1 session
// (PUT /api/connections over HTTPS), then streams MSG_GAMEPAD_DATA packets to the
// UDP data plane while a co-located XInput reader watches the resulting ViGEm
// virtual pad. Sender and reader share one QueryPerformanceCounter clock, so the
// ENTIRE receive-side chain is timed end to end without any cross-device sync:
//
//   sendto -> [loopback UDP] -> recvfrom -> decrypt -> parse -> ViGEm submit
//          -> ViGEmBus driver -> XInput-visible
//
// Each packet carries a 16-bit sequence number embedded in the right-stick X axis
// (an axis the satellite passes through verbatim). The reader decodes that axis on
// every XInput packet-number change, matches it to the send timestamp, and records
// the delta. The same change events give the inter-update interval -> the exact
// metric cakama3a/Polling reports ("<3 ms").
//
// It also polls the admin telemetry (GET 127.0.0.1:9877/api/debug) for the
// server's own recv->submit timing (g_lastLoopUs) so stage 3 (server CPU work) can
// be split out from stage 4 (driver -> XInput-visible).
//
// Stages measured here (run sender+reader on the satellite PC -> loopback, net~=0):
//   stage 3  recv -> ViGEm submit returns      : telemetry lastLoopUs
//   stage 4  submit -> XInput-visible          : end_to_end - stage3 (derived)
//   poll     XInput inter-update interval        : cakama3a-equivalent cadence
//   e2e      sendto -> XInput-visible           : full receive-side pipeline
//
// The device-side stages (0 controller/USB poll, 1 Android hot path, 2 network)
// are measured separately on the phone; see tools/bench/README.md.
//
// Build: tools/bench/build.sh   (ucrt64 g++)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <xinput.h>

#include <sodium.h>

#include "net/session_crypto.h" // deriveSessionKey, computeHmacProof, encryptPacket
#include "core/types.h"         // GamepadReport, CRYPTO_*, MSG sizes

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---- protocol constants (MSG_GAMEPAD_DATA comes from core/types.h) ----------
static constexpr int OUTER_HEADER = 8; // token(4 BE) + counter(4 BE)
static constexpr int INNER_HEADER = 4; // msgType(2 BE) + msgLen(2 BE)

// ---- timing -----------------------------------------------------------------
static double g_qpcFreq = 0.0;
static inline double nowUs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e6 / g_qpcFreq;
}

// ---- small helpers ----------------------------------------------------------
static void putBE16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void putBE32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool hexToBytes(const std::string& hex, uint8_t* out, size_t outLen) {
    if (hex.size() != outLen * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < outLen; i++) {
        int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// crude JSON scalar extractors (the responses are flat + machine-generated)
static std::string jsonStr(const std::string& body, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t k = body.find(pat);
    if (k == std::string::npos) return "";
    size_t c = body.find(':', k + pat.size());
    if (c == std::string::npos) return "";
    size_t q1 = body.find('"', c + 1);
    if (q1 == std::string::npos) return "";
    size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return body.substr(q1 + 1, q2 - q1 - 1);
}
static long long jsonNum(const std::string& body, const std::string& key, long long dflt) {
    std::string pat = "\"" + key + "\"";
    size_t k = body.find(pat);
    if (k == std::string::npos) return dflt;
    size_t c = body.find(':', k + pat.size());
    if (c == std::string::npos) return dflt;
    size_t i = c + 1;
    while (i < body.size() && (body[i] == ' ' || body[i] == '"')) i++;
    bool neg = (i < body.size() && body[i] == '-');
    if (neg) i++;
    long long v = 0;
    bool any = false;
    while (i < body.size() && body[i] >= '0' && body[i] <= '9') {
        v = v * 10 + (body[i] - '0');
        i++;
        any = true;
    }
    if (!any) return dflt;
    return neg ? -v : v;
}

// ---- WinHTTP request (handles both HTTPS client API and plain admin API) -----
static bool httpRequest(bool secure, const std::string& host, int port, const std::string& method,
                        const std::string& path, const std::string& body, int& outStatus,
                        std::string& outBody) {
    outStatus = 0;
    outBody.clear();
    HINTERNET hSession = WinHttpOpen(L"dish_bench/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    HINTERNET hConnect = WinHttpConnect(hSession, widen(host).c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }
    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq =
        WinHttpOpenRequest(hConnect, widen(method).c_str(), widen(path).c_str(), nullptr,
                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    if (secure) {
        DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                    SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
    }
    std::wstring headers = L"Content-Type: application/json\r\n";
    BOOL ok = WinHttpSendRequest(hReq, headers.c_str(), (DWORD)-1L,
                                 body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                 (DWORD)body.size(), (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);
    if (ok) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        outStatus = (int)code;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(hReq, &chunk[0], avail, &read)) break;
            outBody.append(chunk.data(), read);
        }
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok != FALSE;
}

// ---- XInput via dynamic load (avoids any import-lib ambiguity) ---------------
typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
static PFN_XInputGetState g_XInputGetState = nullptr;
static bool loadXInput() {
    const wchar_t* dlls[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (auto d : dlls) {
        HMODULE m = LoadLibraryW(d);
        if (m) {
            // Through void (*)(), GCC's match-anything function type, as
            // procAddress in app_lifecycle.cpp does for -Wcast-function-type.
            using Anything = void (*)();
            g_XInputGetState = reinterpret_cast<PFN_XInputGetState>(
                reinterpret_cast<Anything>(GetProcAddress(m, "XInputGetState")));
            if (g_XInputGetState) return true;
        }
    }
    return false;
}

// ---- shared state between sender + reader ------------------------------------
static std::atomic<bool> g_running{true};
static double g_sendTs[65536];          // sendTs[seq] = QPC us at sendto
static std::atomic<int> g_padIndex{-1}; // discovered XInput slot

struct Stats {
    std::vector<double> e2e;     // sendto -> XInput-visible (us)
    std::vector<double> pollIvl; // XInput inter-update interval (us)
};
static Stats g_stats;
static std::mutex g_statsMtx;

// ---- percentile reporting ----------------------------------------------------
struct Pctl {
    double n, mn, p50, p90, p99, mx, mean;
};
static Pctl pctl(std::vector<double> v) {
    Pctl r{0, 0, 0, 0, 0, 0, 0};
    if (v.empty()) return r;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) {
        size_t i = (size_t)std::floor(q * (v.size() - 1) + 0.5);
        return v[std::min(i, v.size() - 1)];
    };
    double sum = 0;
    for (double x : v) sum += x;
    r.n = (double)v.size();
    r.mn = v.front();
    r.mx = v.back();
    r.p50 = at(0.50);
    r.p90 = at(0.90);
    r.p99 = at(0.99);
    r.mean = sum / v.size();
    return r;
}

// =============================================================================
// XInput reader thread: find the pad, then record every state change.
// =============================================================================
static void readerThread() {
    int idx = -1;
    DWORD lastPkt = 0;
    double lastSeenUs = 0;
    // discover which XInput slot is live
    while (g_running.load() && idx < 0) {
        for (int i = 0; i < 4; i++) {
            XINPUT_STATE s{};
            if (g_XInputGetState((DWORD)i, &s) == ERROR_SUCCESS) {
                idx = i;
                lastPkt = s.dwPacketNumber;
                g_padIndex.store(i);
                break;
            }
        }
        if (idx < 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (idx < 0) return;

    // tight read loop: spin so we don't under-sample the pad's update cadence
    while (g_running.load()) {
        XINPUT_STATE s{};
        if (g_XInputGetState((DWORD)idx, &s) != ERROR_SUCCESS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (s.dwPacketNumber != lastPkt) {
            double t = nowUs();
            lastPkt = s.dwPacketNumber;
            uint16_t seq = (uint16_t)s.Gamepad.sThumbRX; // we encoded seq here
            double sent = g_sendTs[seq];
            std::lock_guard<std::mutex> lk(g_statsMtx);
            if (sent > 0) {
                double d = t - sent;
                if (d >= 0 && d < 2e6) g_stats.e2e.push_back(d); // sanity < 2 s
            }
            if (lastSeenUs > 0) g_stats.pollIvl.push_back(t - lastSeenUs);
            lastSeenUs = t;
        }
        YieldProcessor();
    }
}

// =============================================================================
// telemetry thread: poll /api/debug for the server's recv->submit timing.
// =============================================================================
static std::vector<double> g_stage3Us; // server lastLoopUs samples
static long long g_finalSubmitOk = 0, g_finalDecryptFail = 0, g_finalReplay = 0, g_finalMaxLoop = 0;
static void telemetryThread(const std::string& adminHost, int adminPort) {
    while (g_running.load()) {
        int st = 0;
        std::string body;
        if (httpRequest(false, adminHost, adminPort, "GET", "/api/debug", "", st, body) &&
            st == 200) {
            long long last = jsonNum(body, "lastLoopUs", -1);
            if (last >= 0) g_stage3Us.push_back((double)last);
            g_finalSubmitOk = jsonNum(body, "submitOk", g_finalSubmitOk);
            g_finalDecryptFail = jsonNum(body, "decryptFail", g_finalDecryptFail);
            g_finalReplay = jsonNum(body, "replayDrop", g_finalReplay);
            long long mx = jsonNum(body, "maxLoopUs", 0); // exchange(0) on server read
            if (mx > g_finalMaxLoop) g_finalMaxLoop = mx;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

// =============================================================================
// session setup over HTTPS
// =============================================================================
struct Session {
    uint32_t token = 0;
    uint8_t sessionKey[CRYPTO_KEY_SIZE]{};
    bool ok = false;
};
static Session openSession(const std::string& host, const std::string& deviceId,
                           const uint8_t pairingKey[CRYPTO_KEY_SIZE]) {
    Session s;
    std::string proof = computeHmacProof(pairingKey, deviceId);
    // NOTE: deviceName is intentionally omitted. The server falls back to the
    // paired record's existing name, so a bench run never renames a real device.
    std::string body =
        "{\"protocolVersion\":1,"
        "\"deviceId\":\"" +
        deviceId + "\",\"hmacProof\":\"" + proof +
        "\","
        "\"controllers\":[{\"ctrlIdx\":0,\"type\":0,"
        "\"caps\":{\"rumble\":false,\"motion\":false,\"analogTriggers\":false,\"lightbar\":false},"
        "\"touchpadMode\":\"off\"}],"
        "\"hostFeatures\":{\"mouseControl\":false}}";
    // proof also goes in headers (server accepts header or body)
    int st = 0;
    std::string resp;
    // X-Device-Id / X-Hmac-Proof are read from headers; inject via a manual header line.
    // httpRequest only sets Content-Type, so fold the auth headers into the body-path:
    // satellite reads them from headers, so we re-issue with explicit headers here.
    {
        HINTERNET hS = WinHttpOpen(L"dish_bench/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET hC =
            WinHttpConnect(hS, widen(host).c_str(), (INTERNET_PORT)DEFAULT_CLIENT_PORT, 0);
        HINTERNET hR =
            WinHttpOpenRequest(hC, L"PUT", L"/api/connections", nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                    SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hR, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
        std::wstring hdr = L"Content-Type: application/json\r\nX-Device-Id: " + widen(deviceId) +
                           L"\r\nX-Hmac-Proof: " + widen(proof) + L"\r\n";
        BOOL ok = WinHttpSendRequest(hR, hdr.c_str(), (DWORD)-1L, (LPVOID)body.data(),
                                     (DWORD)body.size(), (DWORD)body.size(), 0);
        if (ok) ok = WinHttpReceiveResponse(hR, nullptr);
        if (ok) {
            DWORD code = 0, len = sizeof(code);
            WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
            st = (int)code;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(hR, &avail) || avail == 0) break;
                std::string chunk(avail, '\0');
                DWORD rd = 0;
                if (!WinHttpReadData(hR, &chunk[0], avail, &rd)) break;
                resp.append(chunk.data(), rd);
            }
        }
        WinHttpCloseHandle(hR);
        WinHttpCloseHandle(hC);
        WinHttpCloseHandle(hS);
    }
    if (st != 200) {
        fprintf(stderr, "[session] PUT /api/connections failed: HTTP %d\n%s\n", st, resp.c_str());
        return s;
    }
    std::string tokenHex = jsonStr(resp, "token");
    std::string saltHex = jsonStr(resp, "sessionSalt");
    uint32_t token = 0;
    if (tokenHex.size() != 8 || sscanf(tokenHex.c_str(), "%08x", &token) != 1) {
        fprintf(stderr, "[session] bad token in response: %s\n", resp.c_str());
        return s;
    }
    uint8_t salt[SESSION_SALT_SIZE]{};
    if (!hexToBytes(saltHex, salt, SESSION_SALT_SIZE)) {
        fprintf(stderr, "[session] bad sessionSalt in response: %s\n", resp.c_str());
        return s;
    }
    s.token = token;
    deriveSessionKey(pairingKey, salt, token, s.sessionKey);
    s.ok = true;
    fprintf(stderr, "[session] open: token=%08x connectionId=%s\n", token,
            jsonStr(resp, "connectionId").c_str());
    return s;
}

// build + encrypt one MSG_GAMEPAD_DATA packet, return wire bytes
static size_t buildPacket(const Session& s, uint32_t counter, uint16_t seq, uint8_t* out) {
    uint8_t inner[INNER_HEADER + 1 + sizeof(GamepadReport)];
    putBE16(inner, MSG_GAMEPAD_DATA);
    putBE16(inner + 2, (uint16_t)(1 + sizeof(GamepadReport)));
    inner[INNER_HEADER + 0] = 0; // ctrlIdx 0
    GamepadReport r{};
    r.sThumbRX = (int16_t)seq; // the correlation key the reader decodes
    r.sThumbLX = (int16_t)seq; // redundant copy
    std::memcpy(inner + INNER_HEADER + 1, &r, sizeof(r));

    uint8_t cipher[sizeof(inner) + 16];
    unsigned long long clen = 0;
    if (!encryptPacket(s.sessionKey, CRYPTO_DIR_CLIENT_TO_SERVER, counter, s.token, inner,
                       sizeof(inner), cipher, &clen))
        return 0;
    putBE32(out, s.token);
    putBE32(out + 4, counter);
    std::memcpy(out + OUTER_HEADER, cipher, (size_t)clen);
    return OUTER_HEADER + (size_t)clen;
}

// =============================================================================
// crypto self-test against the pinned interop vector (no server needed)
// =============================================================================
static int selftest() {
    if (sodium_init() < 0) {
        printf("sodium_init failed\n");
        return 1;
    }
    uint8_t pk[CRYPTO_KEY_SIZE];
    for (int i = 0; i < CRYPTO_KEY_SIZE; i++) pk[i] = (uint8_t)(i + 1);
    uint8_t salt[SESSION_SALT_SIZE] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18};
    uint8_t key[CRYPTO_KEY_SIZE];
    deriveSessionKey(pk, salt, 0x12345678u, key);
    char hex[CRYPTO_KEY_SIZE * 2 + 1];
    sodium_bin2hex(hex, sizeof(hex), key, sizeof(key));
    const char* want = "946f704cf07e2dde5e9995a70d3d103753b4687a7ed9656bc6481b06065a8584";
    printf("deriveSessionKey: %s\n", hex);
    printf("expected        : %s\n", want);
    bool keyOk = std::string(hex) == want;
    std::string proof = computeHmacProof(pk, "device-1");
    const char* wantProof = "05a035a10c55fdfe254c9df5df55a614ac128b123a5de225ea33b41f1d4eedde";
    printf("hmacProof       : %s\n", proof.c_str());
    printf("expected        : %s\n", wantProof);
    bool proofOk = proof == wantProof;
    printf("%s\n", (keyOk && proofOk) ? "SELFTEST PASS" : "SELFTEST FAIL");
    return (keyOk && proofOk) ? 0 : 1;
}

// read first paired (deviceId, sharedKey) from satellite config.json
static bool readConfigDevice(std::string& deviceId, std::string& keyHex) {
    const char* appdata = getenv("APPDATA");
    if (!appdata) return false;
    std::string path = std::string(appdata) + "\\satellite\\config.json";
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "[config] not found: %s\n", path.c_str());
        return false;
    }
    std::string text;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), fp)) > 0) text.append(buf, got);
    fclose(fp);
    size_t arr = text.find("\"pairedDevices\"");
    if (arr == std::string::npos) return false;
    // walk forward: first "id" then first "sharedKey" after it
    size_t idp = text.find("\"id\"", arr);
    size_t skp = text.find("\"sharedKey\"", arr);
    if (idp == std::string::npos || skp == std::string::npos) return false;
    std::string tail = text.substr(arr);
    deviceId = jsonStr(tail, "id");
    keyHex = jsonStr(tail, "sharedKey");
    return !deviceId.empty() && keyHex.size() == CRYPTO_KEY_SIZE * 2;
}

static void usage() {
    printf(
        "dish_bench - satellite receive-path latency benchmark\n"
        "  dish_bench selftest                 validate crypto vs pinned vector (no server)\n"
        "  dish_bench run [opts]               run the benchmark\n"
        "options for run:\n"
        "  --host <ip>        satellite host (default 127.0.0.1; use the PC's LAN IP if remote)\n"
        "  --admin-host <ip>  admin telemetry host (default 127.0.0.1)\n"
        "  --device-id <s>    paired deviceId (default: first in config.json)\n"
        "  --key <hex64>      32-byte pairing key hex (default: from config.json)\n"
        "  --rate <hz>        send rate (default 500)\n"
        "  --secs <n>         duration seconds (default 10)\n"
        "  --csv <path>       write per-sample CSV\n");
}

// precise pacer: timeBeginPeriod(1) + spin tail
static void paceTo(double targetUs) {
    for (;;) {
        double r = targetUs - nowUs();
        if (r <= 0) return;
        if (r > 1500)
            Sleep(1);
        else
            YieldProcessor();
    }
}

int runBench(int argc, char** argv) {
    std::string host = "127.0.0.1", adminHost = "127.0.0.1", deviceId, keyHex, csvPath;
    int rate = 500, secs = 10;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--host")
            host = next();
        else if (a == "--admin-host")
            adminHost = next();
        else if (a == "--device-id")
            deviceId = next();
        else if (a == "--key")
            keyHex = next();
        else if (a == "--rate")
            rate = std::stoi(next());
        else if (a == "--secs")
            secs = std::stoi(next());
        else if (a == "--csv")
            csvPath = next();
    }
    if (sodium_init() < 0) {
        fprintf(stderr, "sodium_init failed\n");
        return 1;
    }
    if (deviceId.empty() || keyHex.empty()) {
        std::string cid, ckey;
        if (readConfigDevice(cid, ckey)) {
            if (deviceId.empty()) deviceId = cid;
            if (keyHex.empty()) keyHex = ckey;
            fprintf(stderr, "[config] using paired device '%s'\n", deviceId.c_str());
        }
    }
    uint8_t pairingKey[CRYPTO_KEY_SIZE]{};
    if (!hexToBytes(keyHex, pairingKey, CRYPTO_KEY_SIZE)) {
        fprintf(stderr, "no usable pairing key (pass --key <hex64> or pair a device first)\n");
        return 1;
    }
    if (!loadXInput())
        fprintf(stderr, "[warn] XInput DLL not found; stage-4/poll metrics disabled\n");

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = (double)f.QuadPart;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    Session s = openSession(host, deviceId, pairingKey);
    if (!s.ok) return 1;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons((u_short)DEFAULT_UDP_PORT);
    inet_pton(AF_INET, host.c_str(), &dst.sin_addr);

    std::memset(g_sendTs, 0, sizeof(g_sendTs));
    std::thread reader, telem(telemetryThread, adminHost, DEFAULT_WEB_PORT);
    if (g_XInputGetState) reader = std::thread(readerThread);

    timeBeginPeriod(1);
    double startUs = nowUs();
    double interval = 1e6 / rate;
    uint32_t counter = 1;
    uint16_t seq = 1;
    uint8_t pkt[OUTER_HEADER + INNER_HEADER + 1 + sizeof(GamepadReport) + 16];
    long long sent = 0;
    double nextUs = startUs;
    fprintf(stderr, "[run] streaming %d Hz for %d s to %s:%d ...\n", rate, secs, host.c_str(),
            DEFAULT_UDP_PORT);
    while (nowUs() - startUs < secs * 1e6) {
        size_t len = buildPacket(s, counter, seq, pkt);
        g_sendTs[seq] = nowUs();
        sendto(sock, (const char*)pkt, (int)len, 0, (sockaddr*)&dst, sizeof(dst));
        counter++;
        seq++;
        if (seq == 0) seq = 1; // skip 0 so sendTs[0]==unused sentinel stays clean
        sent++;
        nextUs += interval;
        paceTo(nextUs);
    }
    timeEndPeriod(1);
    // let the last packets drain through the pipeline + one telemetry poll
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    g_running.store(false);
    if (reader.joinable()) reader.join();
    if (telem.joinable()) telem.join();
    closesocket(sock);
    WSACleanup();

    // ---- report ----
    Pctl e2e = pctl(g_stats.e2e);
    Pctl poll = pctl(g_stats.pollIvl);
    Pctl stg3 = pctl(g_stage3Us);

    printf("\n================ dish_bench results ================\n");
    printf("sent packets        : %lld @ %d Hz over %d s\n", sent, rate, secs);
    printf("server submitOk     : %lld   decryptFail: %lld   replayDrop: %lld\n", g_finalSubmitOk,
           g_finalDecryptFail, g_finalReplay);
    printf("XInput pad slot     : %d\n", g_padIndex.load());
    auto line = [](const char* name, const char* unit, const Pctl& p) {
        if (p.n == 0) {
            printf("  %-30s (no samples)\n", name);
            return;
        }
        printf("  %-30s n=%-6.0f min=%.3f p50=%.3f p90=%.3f p99=%.3f max=%.3f mean=%.3f %s\n", name,
               p.n, p.mn, p.p50, p.p90, p.p99, p.mx, p.mean, unit);
    };
    printf("\n-- receive-side chain (one PC, one clock; loopback => network~=0) --\n");
    line("stage3 recv->ViGEm submit", "ms",
         {stg3.n, stg3.mn / 1000, stg3.p50 / 1000, stg3.p90 / 1000, stg3.p99 / 1000, stg3.mx / 1000,
          stg3.mean / 1000});
    line("e2e sendto->XInput-visible", "ms",
         {e2e.n, e2e.mn / 1000, e2e.p50 / 1000, e2e.p90 / 1000, e2e.p99 / 1000, e2e.mx / 1000,
          e2e.mean / 1000});
    if (e2e.n > 0 && stg3.n > 0) {
        double s4 = (e2e.p50 - stg3.p50) / 1000.0;
        printf("  %-30s ~= %.3f ms (e2e p50 - stage3 p50)\n", "stage4 submit->visible (derived)",
               s4 < 0 ? 0 : s4);
    }
    printf("\n-- poll cadence as the PC sees it (cakama3a/Polling equivalent) --\n");
    line("XInput inter-update interval", "ms",
         {poll.n, poll.mn / 1000, poll.p50 / 1000, poll.p90 / 1000, poll.p99 / 1000, poll.mx / 1000,
          poll.mean / 1000});
    if (poll.n > 0 && poll.p50 > 0)
        printf("  %-30s ~= %.0f Hz\n", "effective update rate", 1e6 / poll.p50);
    printf("server maxLoopUs (window): %.3f ms\n", g_finalMaxLoop / 1000.0);
    printf("====================================================\n");

    if (!csvPath.empty()) {
        FILE* c = fopen(csvPath.c_str(), "wb");
        if (c) {
            fprintf(c, "metric,sample_us\n");
            for (double x : g_stats.e2e) fprintf(c, "e2e,%.3f\n", x);
            for (double x : g_stats.pollIvl) fprintf(c, "poll,%.3f\n", x);
            for (double x : g_stage3Us) fprintf(c, "stage3,%.3f\n", x);
            fclose(c);
            fprintf(stderr, "[csv] wrote %s\n", csvPath.c_str());
        } else {
            fprintf(stderr, "[csv] could not open %s\n", csvPath.c_str());
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "selftest") return selftest();
    if (cmd == "run") return runBench(argc, argv);
    usage();
    return 1;
}
