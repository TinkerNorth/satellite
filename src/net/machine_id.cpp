// SPDX-License-Identifier: LGPL-3.0-or-later

#include "machine_id.h"
#include "config.h" // configPath()

#include <sodium.h>

#include <fstream>
#include <mutex>

namespace {
std::mutex g_machineIdMtx;
std::string g_machineId;

// Beside the config file, like tls.cpp's cert: per-install identity that must
// survive restarts.
std::string machineIdPath() {
    std::string p = configPath();
    auto pos = p.find_last_of("/\\");
    return (pos != std::string::npos ? p.substr(0, pos) : std::string(".")) + "/machine-id";
}

std::string toHex(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0F]);
    }
    return out;
}
} // namespace

std::string ensureMachineId() {
    std::lock_guard<std::mutex> lk(g_machineIdMtx);
    if (!g_machineId.empty()) return g_machineId;

    const std::string path = machineIdPath();
    {
        std::ifstream in(path);
        std::string persisted;
        if (in && std::getline(in, persisted)) {
            while (!persisted.empty() && (persisted.back() == '\n' || persisted.back() == '\r' ||
                                          persisted.back() == ' '))
                persisted.pop_back();
            // Regenerate on corruption rather than advertise a junk id.
            if (isValidMachineId(persisted)) {
                g_machineId = persisted;
                return g_machineId;
            }
        }
    }

    uint8_t raw[16];
    randombytes_buf(raw, sizeof(raw));
    g_machineId = toHex(raw, sizeof(raw));

    // Best-effort persist: a failed write still leaves a valid id for this run;
    // the dish re-learns it on the next scan.
    std::ofstream out(path, std::ios::trunc);
    if (out) out << g_machineId << "\n";

    return g_machineId;
}
