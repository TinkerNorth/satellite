/*
 * pairing.cpp — TCP pairing server thread with X25519 key exchange
 */
#include "pairing.h"
#include "crypto.h"
#include "config.h"
#include <sodium.h>

void pairingThread() {
    g_pairSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_pairSock == INVALID_SOCKET) {
        logMsg(LogLevel::ERR, "pairing", "Failed to create pairing socket");
        return;
    }

    int opt = 1;
    setsockopt(g_pairSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt),
               sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_config.pairPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_pairSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        logMsg(LogLevel::ERR, "pairing",
               "Failed to bind pairing socket on port " + std::to_string(g_config.pairPort));
        closesocket(g_pairSock);
        g_pairSock = INVALID_SOCKET;
        return;
    }
    listen(g_pairSock, 5);
    logMsg(LogLevel::INFO, "pairing",
           "Pairing server listening on port " + std::to_string(g_config.pairPort));

    // Non-blocking so we can check g_appRunning
    u_long nonBlock = 1;
    ioctlsocket(g_pairSock, FIONBIO, &nonBlock);

    while (g_appRunning) {
        sockaddr_in client{};
        int clen = sizeof(client);
        SOCKET cs = accept(g_pairSock, reinterpret_cast<sockaddr*>(&client), &clen);
        if (cs == INVALID_SOCKET) {
            Sleep(100);
            continue;
        }

        // Set client socket to blocking with timeout
        u_long blocking = 0;
        ioctlsocket(cs, FIONBIO, &blocking);
        DWORD timeout = 5000;
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout));

        char buf[2048] = {};
        int n = recv(cs, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            std::string msg(buf);
            auto deviceId = jsonGetString(msg, "deviceId");
            auto deviceName = jsonGetString(msg, "deviceName");
            auto pin = jsonGetString(msg, "pin");
            auto clientPkHex = jsonGetString(msg, "publicKey"); // client's X25519 public key

            char clientIP[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client.sin_addr, clientIP, sizeof(clientIP));

            // Check if already paired
            bool alreadyPaired = false;
            {
                std::lock_guard<std::mutex> lk(g_configMtx);
                auto it = std::find_if(g_config.pairedDevices.begin(), g_config.pairedDevices.end(),
                                       [&](const PairedDevice& d) { return d.id == deviceId; });
                if (it != g_config.pairedDevices.end()) {
                    alreadyPaired = true;
                    it->lastIP = clientIP;
                }
            }

            if (alreadyPaired) {
                logMsg(LogLevel::INFO, "pairing",
                       "Device " + deviceName + " (" + std::string(clientIP) +
                           ") already paired, updating IP");
                // Include the shared key so the client can recover from lost key state
                std::string storedKey;
                {
                    std::lock_guard<std::mutex> lk(g_configMtx);
                    for (const auto& d : g_config.pairedDevices) {
                        if (d.id == deviceId) {
                            storedKey = d.sharedKeyHex;
                            break;
                        }
                    }
                    saveConfig(g_config);
                }
                std::string response =
                    R"({"ok":true,"message":"already paired","sharedKey":")" + storedKey + R"("})";
                send(cs, response.c_str(), (int)response.size(), 0);
            } else if (verifyPin(pin)) {
                // Generate server key pair for X25519 key exchange
                uint8_t serverPk[32], serverSk[32];
                generateKeyPair(serverPk, serverSk);

                // Decode client's public key
                uint8_t clientPk[32];
                bool hasClientKey =
                    !clientPkHex.empty() && hexDecode(clientPkHex, clientPk, 32);

                std::string sharedKeyHex;
                if (hasClientKey) {
                    // Compute shared secret via X25519
                    uint8_t sharedKey[32];
                    if (computeSharedKey(sharedKey, clientPk, serverSk, serverPk)) {
                        sharedKeyHex = hexEncode(sharedKey, 32);
                        sodium_memzero(sharedKey, 32);
                    }
                }

                // If no client key provided, generate a random shared key
                if (sharedKeyHex.empty()) {
                    uint8_t randomKey[32];
                    randombytes_buf(randomKey, 32);
                    sharedKeyHex = hexEncode(randomKey, 32);
                    sodium_memzero(randomKey, 32);
                }

                PairedDevice dev;
                dev.id = deviceId;
                dev.name = deviceName.empty() ? ("Device-" + deviceId.substr(0, 8)) : deviceName;
                dev.lastIP = clientIP;
                dev.pairedAt = getCurrentDate();
                dev.sharedKeyHex = sharedKeyHex;
                {
                    std::lock_guard<std::mutex> lk(g_configMtx);
                    // Remove any existing entry for this device to prevent duplicates
                    auto& devs = g_config.pairedDevices;
                    devs.erase(
                        std::remove_if(devs.begin(), devs.end(),
                                       [&](const PairedDevice& d) { return d.id == deviceId; }),
                        devs.end());
                    devs.push_back(dev);
                    saveConfig(g_config);
                }

                // Build response with server public key (for X25519) or shared key (for simple
                // mode)
                std::string serverPkHex = hexEncode(serverPk, 32);
                std::string response;
                if (hasClientKey) {
                    response = R"({"ok":true,"message":"paired successfully","serverPublicKey":")" +
                               serverPkHex + R"("})";
                } else {
                    // No client key exchange — send shared key directly (trusted network mode)
                    response = R"({"ok":true,"message":"paired successfully","sharedKey":")" +
                               sharedKeyHex + R"("})";
                }
                send(cs, response.c_str(), (int)response.size(), 0);
                sodium_memzero(serverSk, 32);
                logMsg(LogLevel::INFO, "pairing",
                       "Successfully paired device: " + dev.name + " (" + std::string(clientIP) +
                           ")");
            } else {
                logMsg(LogLevel::WARN, "pairing",
                       "Invalid PIN attempt from " + std::string(clientIP));
                send(cs, R"({"ok":false,"error":"invalid or expired PIN"})", 46, 0);
            }
        }
        closesocket(cs);
    }

    closesocket(g_pairSock);
    g_pairSock = INVALID_SOCKET;
}
