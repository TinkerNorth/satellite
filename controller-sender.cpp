/*
 * Controller Sender — reads local Xbox controller via XInput, streams over UDP
 *
 * Captures the state of a physical Xbox controller using XInput and sends
 * XUSB_REPORT packets (12 bytes) over UDP to a receiver running on a remote
 * (or local) machine.
 *
 * Before streaming, performs a TCP pairing handshake with the receiver
 * (port 9878). If not already paired, prompts the user for a PIN displayed
 * in the receiver's web UI.
 *
 * Build:  g++ -O2 -o controller-sender.exe controller-sender.cpp
 *              -Ivigem/include -lxinput1_4 -lws2_32
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <fstream>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <xinput.h>
#include <shlobj.h>

// XUSB_REPORT is binary-compatible with XINPUT_GAMEPAD
#pragma pack(push, 1)
struct XUSB_REPORT {
    USHORT wButtons;
    BYTE   bLeftTrigger;
    BYTE   bRightTrigger;
    SHORT  sThumbLX;
    SHORT  sThumbLY;
    SHORT  sThumbRX;
    SHORT  sThumbRY;
};
#pragma pack(pop)
static_assert(sizeof(XUSB_REPORT) == 12, "XUSB_REPORT must be 12 bytes");
static_assert(sizeof(XUSB_REPORT) == sizeof(XINPUT_GAMEPAD), "Must match XINPUT_GAMEPAD");

static volatile bool g_running = true;
static void signalHandler(int) { g_running = false; }

// ── Device identity persistence ─────────────────────────────────────────────

static std::string getConfigDir() {
    char buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        std::string dir = std::string(buf) + "\\controller-forward";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir;
    }
    return ".";
}

static std::string generateDeviceId() {
    char id[33];
    srand((unsigned)GetTickCount() ^ (unsigned)(uintptr_t)&id);
    for (int i = 0; i < 32; i++)
        id[i] = "0123456789abcdef"[rand() % 16];
    id[32] = 0;
    return id;
}

static std::string loadOrCreateDeviceId() {
    std::string path = getConfigDir() + "\\sender-identity.txt";
    std::ifstream f(path);
    if (f.is_open()) {
        std::string id;
        std::getline(f, id);
        if (id.size() >= 16) return id;
    }
    std::string id = generateDeviceId();
    std::ofstream out(path);
    if (out.is_open()) out << id;
    return id;
}

static std::string getDeviceName() {
    char name[256];
    DWORD size = sizeof(name);
    if (GetComputerNameA(name, &size)) return name;
    return "Unknown";
}

// ── TCP Pairing Handshake ───────────────────────────────────────────────────

static bool doPairingHandshake(const char* host, int pairPort,
                                const std::string& deviceId,
                                const std::string& deviceName) {
    auto attempt = [&](const std::string& pin) -> int {
        // 0 = success, 1 = invalid pin, -1 = connection error
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return -1;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)pairPort);
        inet_pton(AF_INET, host, &addr.sin_addr);

        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(s);
            return -1;
        }

        DWORD timeout = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

        std::string msg = "{\"deviceId\":\"" + deviceId
                        + "\",\"deviceName\":\"" + deviceName
                        + "\",\"pin\":\"" + pin + "\"}";
        send(s, msg.c_str(), (int)msg.size(), 0);

        char buf[512] = {};
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        closesocket(s);

        if (n <= 0) return -1;
        buf[n] = 0;
        if (strstr(buf, "\"ok\":true")) return 0;
        return 1;
    };

    printf("[*] Connecting to pairing server %s:%d...\n", host, pairPort);
    int result = attempt("");

    if (result == 0) {
        printf("[+] Already paired with receiver\n");
        return true;
    }
    if (result == -1) {
        printf("[!] Could not connect to pairing server\n");
        printf("[?] Proceed without pairing? (y/n): ");
        char c;
        if (scanf(" %c", &c) != 1) return false;
        return (c == 'y' || c == 'Y');
    }

    // Need PIN
    for (int tries = 0; tries < 3; tries++) {
        printf("[?] Enter PIN from receiver web UI: ");
        char pin[16] = {};
        if (scanf("%15s", pin) != 1) return false;

        result = attempt(pin);
        if (result == 0) {
            printf("[+] Paired successfully!\n");
            return true;
        } else if (result == -1) {
            printf("[!] Connection lost\n");
            return false;
        } else {
            printf("[!] Invalid or expired PIN. ");
            if (tries < 2) printf("Try again.\n");
            else printf("Max attempts reached.\n");
        }
    }
    return false;
}

// ── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const char* host = (argc > 1) ? argv[1] : "127.0.0.1";
    const int   port = (argc > 2) ? atoi(argv[2]) : 9876;
    const int   rate = (argc > 3) ? atoi(argv[3]) : 250;  // Hz
    const int   user = (argc > 4) ? atoi(argv[4]) : 0;    // XInput user index
    const int   pairPort = 9878;

    printf("=== Controller Sender (C++ / XInput -> UDP) ===\n");
    printf("[*] Target: %s:%d  Rate: %d Hz  Controller: %d\n", host, port, rate, user);

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // ── Winsock ──
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        fprintf(stderr, "[!] WSAStartup failed\n"); return 1;
    }

    // ── Pairing handshake ──
    std::string deviceId = loadOrCreateDeviceId();
    std::string deviceName = getDeviceName();
    printf("[*] Device ID: %s  Name: %s\n", deviceId.c_str(), deviceName.c_str());

    if (!doPairingHandshake(host, pairPort, deviceId, deviceName)) {
        printf("[!] Pairing failed. Exiting.\n");
        WSACleanup();
        return 1;
    }

    // ── UDP socket ──
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "[!] socket() failed: %d\n", WSAGetLastError()); return 1;
    }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons((u_short)port);
    inet_pton(AF_INET, host, &dest.sin_addr);

    const DWORD sleepMs = (rate > 0) ? (1000 / rate) : 4;
    printf("[+] Polling every %lu ms — press Ctrl+C to quit\n\n", sleepMs);

    XINPUT_STATE prevState{};
    unsigned long long pktCount = 0;
    bool wasConnected = false;

    while (g_running) {
        XINPUT_STATE state{};
        DWORD result = XInputGetState(user, &state);

        if (result == ERROR_SUCCESS) {
            if (!wasConnected) {
                printf("[+] Controller %d connected\n", user);
                wasConnected = true;
            }

            // Only send if state changed (reduces unnecessary traffic)
            if (state.dwPacketNumber != prevState.dwPacketNumber) {
                // XINPUT_GAMEPAD is layout-compatible with XUSB_REPORT
                sendto(sock, (const char*)&state.Gamepad, sizeof(XINPUT_GAMEPAD),
                       0, (sockaddr*)&dest, sizeof(dest));
                pktCount++;

                if ((pktCount & 0x3F) == 0)
                    printf("\r[*] Sent: %llu  Btns: 0x%04X  LX:%6d LY:%6d  ",
                           pktCount, state.Gamepad.wButtons,
                           state.Gamepad.sThumbLX, state.Gamepad.sThumbLY);

                prevState = state;
            }
        } else {
            if (wasConnected) {
                printf("[!] Controller %d disconnected\n", user);
                wasConnected = false;
            }
        }

        Sleep(sleepMs);
    }

    printf("\n[*] Shutting down...\n");
    closesocket(sock);
    WSACleanup();
    return 0;
}

