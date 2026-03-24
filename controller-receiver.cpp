/*
 * Controller Receiver — low-latency UDP → ViGEmBus Xbox 360 controller
 *
 * Listens on a UDP port for XUSB_REPORT packets (12 bytes) and injects them
 * into Windows as a virtual Xbox 360 controller via the ViGEmBus driver.
 * No external DLLs required — talks to the kernel driver directly.
 *
 * Build:  g++ -O2 -o controller-receiver.exe controller-receiver.cpp
 *              -Ivigem/include -lsetupapi -lws2_32
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <setupapi.h>

#include "ViGEm/BusShared.h"

// ── Globals for clean shutdown ──────────────────────────────────────────────
static volatile bool g_running = true;
static HANDLE        g_busDevice = INVALID_HANDLE_VALUE;
static ULONG         g_serialNo  = 0;

static void signalHandler(int) { g_running = false; }

// ── ViGEmBus helpers ────────────────────────────────────────────────────────

static HANDLE openVigemBus() {
    SP_DEVICE_INTERFACE_DATA did{};
    did.cbSize = sizeof(did);
    DWORD idx = 0, reqSize = 0;

    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_BUSENUM_VIGEM, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    while (SetupDiEnumDeviceInterfaces(devInfo, nullptr,
            &GUID_DEVINTERFACE_BUSENUM_VIGEM, idx++, &did)) {
        SetupDiGetDeviceInterfaceDetailW(devInfo, &did, nullptr, 0, &reqSize, nullptr);
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &did, detail, reqSize, nullptr, nullptr)) {
            HANDLE h = CreateFileW(detail->DevicePath,
                GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
            free(detail);
            if (h != INVALID_HANDLE_VALUE) {
                // Version check
                OVERLAPPED ov{};
                ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                VIGEM_CHECK_VERSION ver;
                VIGEM_CHECK_VERSION_INIT(&ver, VIGEM_COMMON_VERSION);
                DWORD xfr = 0;
                DeviceIoControl(h, IOCTL_VIGEM_CHECK_VERSION, &ver, ver.Size,
                                nullptr, 0, &xfr, &ov);
                if (GetOverlappedResult(h, &ov, &xfr, TRUE)) {
                    CloseHandle(ov.hEvent);
                    SetupDiDestroyDeviceInfoList(devInfo);
                    return h;
                }
                CloseHandle(ov.hEvent);
                CloseHandle(h);
            }
        } else {
            free(detail);
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return INVALID_HANDLE_VALUE;
}

static bool pluginTarget(HANDLE bus, ULONG serial) {
    OVERLAPPED ov{}; ov.hEvent = CreateEvent(nullptr,FALSE,FALSE,nullptr);
    DWORD xfr = 0;
    VIGEM_PLUGIN_TARGET plug;
    VIGEM_PLUGIN_TARGET_INIT(&plug, serial, Xbox360Wired);
    plug.VendorId  = 0x045E;
    plug.ProductId = 0x028E;
    DeviceIoControl(bus, IOCTL_VIGEM_PLUGIN_TARGET, &plug, plug.Size,
                    nullptr, 0, &xfr, &ov);
    bool ok = GetOverlappedResult(bus, &ov, &xfr, TRUE) != 0;
    if (ok) {
        VIGEM_WAIT_DEVICE_READY wr;
        VIGEM_WAIT_DEVICE_READY_INIT(&wr, serial);
        OVERLAPPED ov2{}; ov2.hEvent = CreateEvent(nullptr,FALSE,FALSE,nullptr);
        DeviceIoControl(bus, IOCTL_VIGEM_WAIT_DEVICE_READY, &wr, wr.Size,
                        nullptr, 0, &xfr, &ov2);
        GetOverlappedResult(bus, &ov2, &xfr, TRUE);  // may fail on old drivers, ok
        CloseHandle(ov2.hEvent);
    }
    CloseHandle(ov.hEvent);
    return ok;
}

static bool submitReport(HANDLE bus, ULONG serial, const XUSB_REPORT& rpt) {
    OVERLAPPED ov{}; ov.hEvent = CreateEvent(nullptr,FALSE,FALSE,nullptr);
    DWORD xfr = 0;
    XUSB_SUBMIT_REPORT sr;
    XUSB_SUBMIT_REPORT_INIT(&sr, serial);
    sr.Report = rpt;
    DeviceIoControl(bus, IOCTL_XUSB_SUBMIT_REPORT, &sr, sr.Size,
                    nullptr, 0, &xfr, &ov);
    bool ok = GetOverlappedResult(bus, &ov, &xfr, TRUE) != 0;
    CloseHandle(ov.hEvent);
    return ok;
}

static void unplugTarget(HANDLE bus, ULONG serial) {
    OVERLAPPED ov{}; ov.hEvent = CreateEvent(nullptr,FALSE,FALSE,nullptr);
    DWORD xfr = 0;
    VIGEM_UNPLUG_TARGET up;
    VIGEM_UNPLUG_TARGET_INIT(&up, serial);
    DeviceIoControl(bus, IOCTL_VIGEM_UNPLUG_TARGET, &up, up.Size,
                    nullptr, 0, &xfr, &ov);
    GetOverlappedResult(bus, &ov, &xfr, TRUE);
    CloseHandle(ov.hEvent);
}

// ── Cleanup on exit ─────────────────────────────────────────────────────────
static void cleanup() {
    if (g_serialNo && g_busDevice != INVALID_HANDLE_VALUE) {
        printf("[*] Unplugging virtual controller...\n");
        unplugTarget(g_busDevice, g_serialNo);
    }
    if (g_busDevice != INVALID_HANDLE_VALUE) CloseHandle(g_busDevice);
}

// ── Main ────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    const int port = (argc > 1) ? atoi(argv[1]) : 9876;
    printf("=== Controller Receiver (C++ / ViGEmBus direct) ===\n");
    printf("[*] UDP port: %d\n", port);

    // ── Open ViGEmBus ──
    g_busDevice = openVigemBus();
    if (g_busDevice == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[!] ViGEmBus not found. Install from:\n"
                        "    https://github.com/nefarius/ViGEmBus/releases\n");
        return 1;
    }
    printf("[+] Connected to ViGEmBus\n");

    // ── Plug in virtual Xbox 360 controller ──
    g_serialNo = 1;
    if (!pluginTarget(g_busDevice, g_serialNo)) {
        // try slots 1-16
        bool found = false;
        for (ULONG s = 2; s <= 16; ++s) {
            if (pluginTarget(g_busDevice, s)) { g_serialNo = s; found = true; break; }
        }
        if (!found) {
            fprintf(stderr, "[!] Failed to plug in virtual controller (all slots busy?)\n");
            CloseHandle(g_busDevice);
            return 1;
        }
    }
    printf("[+] Virtual Xbox 360 controller plugged in (slot %lu)\n", g_serialNo);
    atexit(cleanup);
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // ── Winsock init ──
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        fprintf(stderr, "[!] WSAStartup failed\n"); return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "[!] socket() failed: %d\n", WSAGetLastError()); return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[!] bind() failed: %d\n", WSAGetLastError()); return 1;
    }

    // Non-blocking so we can check g_running
    u_long nonBlock = 1;
    ioctlsocket(sock, FIONBIO, &nonBlock);

    printf("[+] Listening on 0.0.0.0:%d — press Ctrl+C to quit\n\n", port);

    XUSB_REPORT report;
    XUSB_REPORT_INIT(&report);
    unsigned long long pktCount = 0;

    while (g_running) {
        sockaddr_in sender{};
        int slen = sizeof(sender);
        char buf[64];
        int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&sender, &slen);

        if (n == sizeof(XUSB_REPORT)) {
            memcpy(&report, buf, sizeof(XUSB_REPORT));
            submitReport(g_busDevice, g_serialNo, report);
            pktCount++;
            if ((pktCount & 0xFF) == 0)  // print every 256 packets
                printf("\r[*] Packets: %llu  Btns: 0x%04X  LX:%6d LY:%6d  ",
                       pktCount, report.wButtons, report.sThumbLX, report.sThumbLY);
        } else if (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
            fprintf(stderr, "[!] recvfrom error: %d\n", WSAGetLastError());
        } else if (n == SOCKET_ERROR) {
            Sleep(1);  // ~1ms idle when no data
        }
    }

    printf("\n[*] Shutting down...\n");
    closesocket(sock);
    WSACleanup();
    return 0;
}
