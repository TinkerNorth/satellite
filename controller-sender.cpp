/*
 * Controller Sender — reads local Xbox controller via XInput, streams over UDP
 *
 * Captures the state of a physical Xbox controller using XInput and sends
 * XUSB_REPORT packets (12 bytes) over UDP to a receiver running on a remote
 * (or local) machine.
 *
 * Build:  g++ -O2 -o controller-sender.exe controller-sender.cpp
 *              -Ivigem/include -lxinput1_4 -lws2_32
 *
 * If xinput1_4 is not available, try: -lxinput9_1_0  or  -lxinput1_3
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <xinput.h>

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

int main(int argc, char* argv[]) {
    const char* host = (argc > 1) ? argv[1] : "127.0.0.1";
    const int   port = (argc > 2) ? atoi(argv[2]) : 9876;
    const int   rate = (argc > 3) ? atoi(argv[3]) : 250;  // Hz
    const int   user = (argc > 4) ? atoi(argv[4]) : 0;    // XInput user index

    printf("=== Controller Sender (C++ / XInput → UDP) ===\n");
    printf("[*] Target: %s:%d  Rate: %d Hz  Controller: %d\n", host, port, rate, user);

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // ── Winsock ──
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        fprintf(stderr, "[!] WSAStartup failed\n"); return 1;
    }

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

