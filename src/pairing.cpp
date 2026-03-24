/*
 * pairing.cpp — TCP pairing server thread
 */
#include "pairing.h"
#include "crypto.h"
#include "config.h"

void pairingThread() {
    g_pairSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_pairSock == INVALID_SOCKET) return;

    int opt = 1;
    setsockopt(g_pairSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_config.pairPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_pairSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(g_pairSock);
        g_pairSock = INVALID_SOCKET;
        return;
    }
    listen(g_pairSock, 5);

    // Non-blocking so we can check g_appRunning
    u_long nonBlock = 1;
    ioctlsocket(g_pairSock, FIONBIO, &nonBlock);

    while (g_appRunning) {
        sockaddr_in client{};
        int clen = sizeof(client);
        SOCKET cs = accept(g_pairSock, (sockaddr*)&client, &clen);
        if (cs == INVALID_SOCKET) {
            Sleep(100);
            continue;
        }

        // Set client socket to blocking with timeout
        u_long blocking = 0;
        ioctlsocket(cs, FIONBIO, &blocking);
        DWORD timeout = 5000;
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

        char buf[1024] = {};
        int n = recv(cs, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            std::string msg(buf);
            auto deviceId = jsonGetString(msg, "deviceId");
            auto deviceName = jsonGetString(msg, "deviceName");
            auto pin = jsonGetString(msg, "pin");

            char clientIP[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client.sin_addr, clientIP, sizeof(clientIP));

            // Check if already paired
            bool alreadyPaired = false;
            {
                std::lock_guard<std::mutex> lk(g_configMtx);
                for (auto& d : g_config.pairedDevices) {
                    if (d.id == deviceId) { alreadyPaired = true; d.lastIP = clientIP; break; }
                }
            }

            if (alreadyPaired) {
                send(cs, R"({"ok":true,"message":"already paired"})", 38, 0);
                std::lock_guard<std::mutex> lk(g_configMtx);
                saveConfig(g_config);
            } else if (verifyPin(pin)) {
                PairedDevice dev;
                dev.id = deviceId;
                dev.name = deviceName.empty() ? ("Device-" + deviceId.substr(0, 8)) : deviceName;
                dev.lastIP = clientIP;
                dev.pairedAt = getCurrentDate();
                {
                    std::lock_guard<std::mutex> lk(g_configMtx);
                    g_config.pairedDevices.push_back(dev);
                    saveConfig(g_config);
                }
                send(cs, R"({"ok":true,"message":"paired successfully"})", 43, 0);
            } else {
                send(cs, R"({"ok":false,"error":"invalid or expired PIN"})", 46, 0);
            }
        }
        closesocket(cs);
    }

    closesocket(g_pairSock);
    g_pairSock = INVALID_SOCKET;
}

