// SPDX-License-Identifier: LGPL-3.0-or-later
// MSG_GAMEPAD_DATA (INPUT) and its protocol-4 tail: the decoder, the dispatch
// path that hands the frame to the backend, and the extended-buttons table
// the contract and the code both carry.
#include "../src/net/inner_dispatch.h"
#include "../src/core/session_service.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "test_util.h"

namespace {

constexpr uint32_t ALL_EXT_BUTTONS = 0x1FFF;
constexpr uint8_t REPORT_FILL = 0x5A;
constexpr uint8_t SLACK_FILL = 0xEE;

struct RecordingGamepad : IGamepadPort {
    bool ensureBusOpen() override { return true; }
    void closeBus() override {}
    bool isBusOpen() const override { return true; }
    bool pluginDevice(uint32_t, GamepadIdentity) override { return true; }
    bool supportsIdentity(GamepadIdentity) const override { return true; }
    bool unplugDevice(uint32_t) override { return true; }
    bool submitReport(uint32_t serial, const GamepadReport& report) override {
        InputReport input;
        input.pad = report;
        return submitInput(serial, input);
    }
    bool submitInput(uint32_t, const InputReport& input) override {
        submits++;
        last = input;
        return true;
    }
    void setRumbleCallback(RumbleCallback) override {}

    int submits = 0;
    InputReport last{};
};

struct SilentClient : IClientPort {
    void updateClientAddr(uint32_t, const std::string&, uint16_t) override {}
    void removeClientAddr(uint32_t) override {}
    void sendHeartbeatAck(const SessionEndpoint&, bool, uint8_t, uint16_t, uint16_t) override {}
    void sendSessionClose(const Connection&, uint8_t) override {}
    void sendRumble(const Connection&, uint8_t, const RumbleReport&) override {}
    void sendLightbar(const Connection&, uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void sendTriggerEffects(const Connection&, uint8_t, const TriggerEffectsReport&) override {}
    void sendPlayerLeds(const Connection&, uint8_t, uint8_t) override {}
    void sendSpeakerAudio(const SessionEndpoint&, uint8_t, uint16_t, const uint8_t*,
                          size_t) override {}
    void sendHapticAudio(const SessionEndpoint&, uint8_t, uint16_t, const uint8_t*,
                         size_t) override {}
    void sendMicLed(const Connection&, uint8_t, uint8_t) override {}
};

struct SilentLog : ILogPort {
    void logMsg(LogLevel, const std::string&, const std::string&) override {}
};

constexpr int PROTOCOL_3 = 3;
constexpr int PROTOCOL_4 = 4;

// One session with a pad at ctrlIdx 0, the shape every dispatch case needs,
// settled at the protocol version the client offered.
struct OnePadSession {
    explicit OnePadSession(int offeredVersion = PROTOCOL_VERSION) : opened(open(offeredVersion)) {}

    RecordingGamepad gamepad;
    SilentClient client;
    SilentLog log;
    SessionService svc{gamepad, client, log};
    SessionUpsertResult opened;

    SessionUpsertResult open(int offeredVersion) {
        const uint8_t key[CRYPTO_KEY_SIZE] = {};
        ControllerDescriptor d;
        d.ctrlIdx = 0;
        d.type = CONTROLLER_TYPE_SWITCHPRO;
        d.caps = CAP_EXTENDED_BUTTONS;
        return svc.upsertSession("dev-input-report", "InputReportTest", "192.168.1.60", key, {d},
                                 false, offeredVersion);
    }

    DispatchResult dispatch(const std::vector<uint8_t>& frame) {
        std::vector<uint8_t> tight(frame);
        return dispatchInnerMessage(svc, opened.token, MSG_GAMEPAD_DATA, tight.data(),
                                    static_cast<uint16_t>(tight.size()));
    }
};

// ctrlIdx, a report of REPORT_FILL bytes, then `tail` (the extended-buttons
// word, or a short piece of one).
std::vector<uint8_t> inputFrame(const std::vector<uint8_t>& tail) {
    std::vector<uint8_t> frame(1 + static_cast<size_t>(INPUT_WIRE_PAYLOAD_BYTES_V1), REPORT_FILL);
    frame[0] = 0;
    frame.insert(frame.end(), tail.begin(), tail.end());
    return frame;
}

std::vector<uint8_t> le32(uint32_t v) {
    return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
            static_cast<uint8_t>(v >> 24)};
}

std::string readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The rows of the contract's extended-buttons table: bit to wire name.
std::map<uint32_t, std::string> contractExtButtons(const std::string& contract) {
    std::map<uint32_t, std::string> rows;
    const size_t start = contract.find("### Extended buttons");
    if (start == std::string::npos) return rows;
    const size_t end = contract.find("\n#", start + 1);
    std::istringstream section(contract.substr(start, end - start));
    const std::regex row(R"(^\| (0x[0-9A-Fa-f]{4}) \| `([A-Za-z0-9]+)` \|)");
    std::string line;
    while (std::getline(section, line)) {
        std::smatch m;
        if (!std::regex_search(line, m, row)) continue;
        rows[static_cast<uint32_t>(std::stoul(m[1].str(), nullptr, 16))] = m[2].str();
    }
    return rows;
}

void testDecoder() {
    TEST("decodeInputReport: a 13-byte frame (protocol 1..3) carries no extended buttons");
    const std::vector<uint8_t> v1 = inputFrame({});
    const InputReport r1 = decodeInputReport(v1.data() + 1, v1.size() - 1);
    EXPECT_EQ(r1.extButtons, 0u);
    EXPECT_EQ(r1.pad.wButtons, static_cast<uint16_t>(0x5A5A));
    EXPECT_EQ(r1.pad.sThumbRY, static_cast<int16_t>(0x5A5A));

    TEST("decodeInputReport: a 17-byte frame (protocol 4) reads the word little-endian");
    const std::vector<uint8_t> v4 = inputFrame(le32(0x00001234));
    const InputReport r4 = decodeInputReport(v4.data() + 1, v4.size() - 1);
    EXPECT_EQ(r4.extButtons, 0x00001234u);
    EXPECT_EQ(r4.pad.bLeftTrigger, REPORT_FILL);

    TEST("decodeInputReport: a 14..16-byte frame is not a word, and reads as none");
    for (size_t extra = 1; extra < 4; extra++) {
        const std::vector<uint8_t> partial = inputFrame(std::vector<uint8_t>(extra, 0xFF));
        EXPECT_EQ(decodeInputReport(partial.data() + 1, partial.size() - 1).extButtons, 0u);
    }

    TEST("decodeInputReport: bytes past the word are ignored");
    std::vector<uint8_t> longer = le32(EXT_BUTTON_MISC1);
    longer.push_back(SLACK_FILL);
    const std::vector<uint8_t> oversized = inputFrame(longer);
    EXPECT_EQ(decodeInputReport(oversized.data() + 1, oversized.size() - 1).extButtons,
              EXT_BUTTON_MISC1);
}

void testWireSizes() {
    TEST("INPUT is 13 bytes through protocol 3 and 17 from protocol 4");
    EXPECT_EQ(1 + INPUT_WIRE_PAYLOAD_BYTES_V1, 13);
    EXPECT_EQ(1 + INPUT_WIRE_PAYLOAD_BYTES_V4, 17);
    EXPECT_EQ(INPUT_WIRE_PAYLOAD_BYTES_V1, static_cast<int>(sizeof(GamepadReport)));
}

void testDispatch() {
    TEST("dispatchInnerMessage: a 17-byte INPUT hands the backend its extended buttons");
    OnePadSession s;
    const DispatchResult dr = s.dispatch(inputFrame(le32(EXT_BUTTON_MISC1)));
    EXPECT(dr.wasGamepadData && dr.gamepadOk);
    EXPECT_EQ(s.gamepad.submits, 1);
    EXPECT_EQ(s.gamepad.last.extButtons, EXT_BUTTON_MISC1);
    EXPECT_EQ(s.gamepad.last.pad.wButtons, static_cast<uint16_t>(0x5A5A));

    TEST("dispatchInnerMessage: 13..16-byte INPUT hands the backend no extended buttons");
    for (size_t extra = 0; extra < 4; extra++) {
        OnePadSession older;
        const DispatchResult partial =
            older.dispatch(inputFrame(std::vector<uint8_t>(extra, 0xFF)));
        EXPECT(partial.wasGamepadData && partial.gamepadOk);
        EXPECT_EQ(older.gamepad.submits, 1);
        EXPECT_EQ(older.gamepad.last.extButtons, 0u);
    }

    TEST("dispatchInnerMessage: both INPUT lengths interleave on one session, as levels");
    OnePadSession mixed;
    mixed.dispatch(inputFrame(le32(EXT_BUTTON_RIGHT_PADDLE1 | EXT_BUTTON_LEFT_PADDLE1)));
    EXPECT_EQ(mixed.gamepad.last.extButtons, EXT_BUTTON_RIGHT_PADDLE1 | EXT_BUTTON_LEFT_PADDLE1);
    mixed.dispatch(inputFrame({}));
    EXPECT_EQ(mixed.gamepad.last.extButtons, 0u);
    mixed.dispatch(inputFrame(le32(EXT_BUTTON_LEFT_PADDLE2)));
    EXPECT_EQ(mixed.gamepad.last.extButtons, EXT_BUTTON_LEFT_PADDLE2);
    EXPECT_EQ(mixed.gamepad.submits, 3);

    TEST("dispatchInnerMessage: a 12-byte INPUT is still malformed");
    OnePadSession truncated;
    const std::vector<uint8_t> runt(static_cast<size_t>(INPUT_WIRE_PAYLOAD_BYTES_V1), 0);
    EXPECT(!truncated.dispatch(runt).wasGamepadData);
    EXPECT_EQ(truncated.gamepad.submits, 0);
}

void testSettledVersionIsNotAGate() {
    TEST("a session settled at protocol 3 that sends 17 bytes has its word read: the receiver "
         "keys on length, not on the settled version");
    OnePadSession v3(PROTOCOL_3);
    EXPECT_EQ(v3.opened.protocolVersion, PROTOCOL_3);
    const DispatchResult tailed = v3.dispatch(inputFrame(le32(EXT_BUTTON_MISC1)));
    EXPECT(tailed.wasGamepadData && tailed.gamepadOk);
    EXPECT_EQ(v3.gamepad.submits, 1);
    EXPECT_EQ(v3.gamepad.last.extButtons, EXT_BUTTON_MISC1);
    EXPECT_EQ(v3.gamepad.last.pad.wButtons, static_cast<uint16_t>(0x5A5A));

    TEST("a session settled at protocol 4 that sends 13 bytes reads no extended buttons");
    OnePadSession v4(PROTOCOL_4);
    EXPECT_EQ(v4.opened.protocolVersion, PROTOCOL_4);
    const DispatchResult bare = v4.dispatch(inputFrame({}));
    EXPECT(bare.wasGamepadData && bare.gamepadOk);
    EXPECT_EQ(v4.gamepad.submits, 1);
    EXPECT_EQ(v4.gamepad.last.extButtons, 0u);
    EXPECT_EQ(v4.gamepad.last.pad.wButtons, static_cast<uint16_t>(0x5A5A));
}

void testExtButtonTable() {
    TEST("the extended buttons are thirteen single bits, in bit order, from 0x0001");
    uint32_t expected = 1;
    uint32_t seen = 0;
    for (const auto& button : EXT_BUTTON_NAMES) {
        EXPECT_EQ(button.bit, expected);
        seen |= button.bit;
        expected <<= 1;
    }
    EXPECT_EQ(seen, ALL_EXT_BUTTONS);

    TEST("docs/contract.md's extended-buttons table is the code's, row for row");
    const std::map<uint32_t, std::string> contract = contractExtButtons(readFile(CONTRACT_MD));
    EXPECT_EQ(contract.size(), sizeof(EXT_BUTTON_NAMES) / sizeof(EXT_BUTTON_NAMES[0]));
    for (const auto& button : EXT_BUTTON_NAMES) {
        const auto row = contract.find(button.bit);
        EXPECT(row != contract.end());
        if (row != contract.end()) EXPECT_EQ(row->second, std::string(button.name));
    }
}

} // namespace

int main() {
    std::cout << "Running INPUT report tests...\n\n";
    testDecoder();
    testWireSizes();
    testDispatch();
    testSettledVersionIsNotAGate();
    testExtButtonTable();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    if (g_fail > 0) {
        std::cout << "  STATUS: FAIL\n";
        return 1;
    }
    std::cout << "  STATUS: ALL PASSED\n";
    return 0;
}
