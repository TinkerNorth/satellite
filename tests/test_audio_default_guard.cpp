// SPDX-License-Identifier: LGPL-3.0-or-later

// platform/windows/audio_default_guard — the controller-audio default-endpoint
// guard's decision layer. Two of these rules are load-bearing in a way a bug
// would be hard to notice: the parent-chain predicate is the ONLY thing keeping
// the guard off a user's real DualSense (identical VID/PID, name, form factor),
// and the IPolicyConfig probe verdict is what stands between us and calling an
// unidentified vtable slot in the audio service. Both get exhaustive coverage
// here, alongside the full restore matrix and the once-per-role-per-plug latch.
// Pure (no <windows.h>), so it runs on every CI platform.
#include "test_util.h"

#include "../src/platform/windows/audio_default_guard.h"

#include <iostream>
#include <string>
#include <vector>

using namespace satellite::audioguard;

namespace {

const char* const SPEAKERS = "{0.0.0.00000000}.{a1b2c3d4-0000-4000-8000-000000000001}";
const char* const HEADSET = "{0.0.0.00000000}.{a1b2c3d4-0000-4000-8000-000000000002}";
const char* const PAD = "{0.0.0.00000000}.{a1b2c3d4-0000-4000-8000-0000000000ff}";

RoleSnapshot captured(const std::string& priorId, bool priorWasOurs = false,
                      bool restored = false) {
    RoleSnapshot s;
    s.captured = true;
    s.priorId = priorId;
    s.priorWasOurs = priorWasOurs;
    s.restored = restored;
    return s;
}

RoleObservation seeing(const std::string& currentId, bool isOurs) {
    RoleObservation o;
    o.haveCurrent = true;
    o.currentId = currentId;
    o.currentIsOurs = isOurs;
    return o;
}

DevNode node(const std::string& instanceId, const std::string& service,
             std::vector<std::string> hardwareIds = {}) {
    DevNode n;
    n.instanceId = instanceId;
    n.service = service;
    n.hardwareIds = std::move(hardwareIds);
    return n;
}

// The chain a composite persona actually produces: endpoint interface, the
// composite parent, then HIDMaestro's usbip-win2 root devnode.
std::vector<DevNode> oursChain() {
    return {
        node("USB\\VID_054C&PID_0CE6&MI_00\\3&253044F8&0&0000", "usbaudio"),
        node("USB\\VID_054C&PID_0CE6\\SATELLITE0001", "usbccgp"),
        node("ROOT\\USB\\0000", "usbip2_ude", {"ROOT\\HIDMAESTRO_UDE", "ROOT\\USBIP2_UDE"}),
    };
}

// A physical DualSense: byte-identical at the top, terminating at the host
// controller instead.
std::vector<DevNode> realDualSenseChain() {
    return {
        node("USB\\VID_054C&PID_0CE6&MI_00\\3&253044F8&0&0000", "usbaudio"),
        node("USB\\VID_054C&PID_0CE6\\9&1D5A2F13&0&4", "usbccgp"),
        node("USB\\ROOT_HUB30\\4&2A1B0C7D&0", "USBHUB3", {"USB\\ROOT_HUB30"}),
        node("PCI\\VEN_1022&DEV_43EE&SUBSYS_11421B21&REV_01\\4&1E0F2C3D&0&0041", "USBXHCI",
             {"PCI\\VEN_1022&DEV_43EE&SUBSYS_11421B21&REV_01"}),
        node("ACPI\\PNP0A08\\0", "pci", {"ACPI\\PNP0A08"}),
    };
}

PolicyConfigProbe goodProbe() {
    PolicyConfigProbe p;
    p.coCreateOk = true;
    p.probeHr = 0;
    p.defaultPeriod = 100000;
    p.minPeriod = 30000;
    return p;
}

} // namespace

static void test_roles_match_the_erole_enum() {
    TEST("Role values are mmdeviceapi ERole, which the IO shell casts straight through");
    EXPECT_EQ(static_cast<int>(Role::Console), 0);
    EXPECT_EQ(static_cast<int>(Role::Multimedia), 1);
    EXPECT_EQ(static_cast<int>(Role::Communications), 2);
    EXPECT_EQ(ROLE_COUNT, (size_t)3);

    TEST("roleIndex covers every slot exactly once");
    EXPECT_EQ(roleIndex(Role::Console), (size_t)0);
    EXPECT_EQ(roleIndex(Role::Multimedia), (size_t)1);
    EXPECT_EQ(roleIndex(Role::Communications), (size_t)2);
    for (size_t i = 0; i < ROLE_COUNT; ++i) EXPECT_EQ(roleIndex(ALL_ROLES[i]), i);

    TEST("role names are stable log tokens");
    EXPECT_EQ(std::string(roleName(Role::Console)), std::string("console"));
    EXPECT_EQ(std::string(roleName(Role::Multimedia)), std::string("multimedia"));
    EXPECT_EQ(std::string(roleName(Role::Communications)), std::string("communications"));
    EXPECT_EQ(std::string(roleName(static_cast<Role>(99))), std::string("?"));
}

static void test_normalize_device_id() {
    TEST("normalisation lowercases ASCII and leaves the rest of the id intact");
    EXPECT_EQ(normalizeDeviceId("USB\\VID_054C&PID_0CE6"), std::string("usb\\vid_054c&pid_0ce6"));
    EXPECT_EQ(normalizeDeviceId("already\\lower_0123"), std::string("already\\lower_0123"));
    EXPECT_EQ(normalizeDeviceId(""), std::string(""));

    TEST("surrounding whitespace and NULs are trimmed (REG_SZ reads carry them)");
    EXPECT_EQ(normalizeDeviceId("  ROOT\\USB\\0000  "), std::string("root\\usb\\0000"));
    EXPECT_EQ(normalizeDeviceId(std::string("ROOT\\USB\\0000\0", 15)),
              std::string("root\\usb\\0000"));
    EXPECT_EQ(normalizeDeviceId("\t\r\n"), std::string(""));
    EXPECT_EQ(normalizeDeviceId(std::string("\0\0", 2)), std::string(""));

    TEST("interior spacing is not touched; only the ends are trimmed");
    EXPECT_EQ(normalizeDeviceId(" A B "), std::string("a b"));
}

static void test_device_ids_equal_is_case_insensitive() {
    TEST("Windows device ids compare case-insensitively");
    EXPECT(deviceIdsEqual("USB\\VID_054C", "usb\\vid_054c"));
    EXPECT(deviceIdsEqual("{0.0.0.00000000}.{ABCD}", "{0.0.0.00000000}.{abcd}"));
    EXPECT(deviceIdsEqual(" pad ", "PAD"));

    TEST("different ids stay different");
    EXPECT(!deviceIdsEqual(SPEAKERS, HEADSET));
    EXPECT(!deviceIdsEqual("usb\\vid_054c", "usb\\vid_054d"));
    EXPECT(!deviceIdsEqual("abc", "abcd"));

    TEST("two empties compare equal; decideRestore rejects emptiness before it asks");
    EXPECT(deviceIdsEqual("", ""));
    EXPECT(deviceIdsEqual("", "   "));
    EXPECT(!deviceIdsEqual("", SPEAKERS));
}

static void test_device_id_contains() {
    TEST("substring match is case-insensitive on both sides");
    EXPECT(deviceIdContains("ROOT\\HIDMAESTRO_UDE", "root\\hidmaestro_ude"));
    EXPECT(deviceIdContains("root\\hidmaestro_ude\\0000", "ROOT\\HIDMAESTRO_UDE"));
    EXPECT(!deviceIdContains("ROOT\\HIDMAESTR0_UDE", "ROOT\\HIDMAESTRO_UDE"));

    TEST("an empty needle never matches, so a missing property cannot claim the device");
    EXPECT(!deviceIdContains("ROOT\\HIDMAESTRO_UDE", ""));
    EXPECT(!deviceIdContains("", ""));
    EXPECT(!deviceIdContains("", "ROOT\\HIDMAESTRO_UDE"));
}

static void test_strip_ks_filter_prefix() {
    TEST("the {n}. topology prefix comes off, leaving a device instance id");
    EXPECT_EQ(stripKsFilterPrefix("{1}.USB\\VID_054C&PID_0CE6&MI_00\\3&253044F8&0&0000"),
              std::string("USB\\VID_054C&PID_0CE6&MI_00\\3&253044F8&0&0000"));
    EXPECT_EQ(stripKsFilterPrefix("{2}.HDAUDIO\\FUNC_01&VEN_10DE"),
              std::string("HDAUDIO\\FUNC_01&VEN_10DE"));
    EXPECT_EQ(stripKsFilterPrefix("{12}.X"), std::string("X"));
    EXPECT_EQ(stripKsFilterPrefix("{0}.A"), std::string("A"));

    TEST("only the FIRST prefix is stripped; dots inside the id survive");
    EXPECT_EQ(stripKsFilterPrefix("{1}.USB\\A.B.C"), std::string("USB\\A.B.C"));
    EXPECT_EQ(stripKsFilterPrefix("{1}.{2}.X"), std::string("{2}.X"));

    TEST("an empty tail is an empty id, not the original string");
    EXPECT_EQ(stripKsFilterPrefix("{1}."), std::string(""));

    TEST("anything that is not exactly {digits}. is returned untouched");
    EXPECT_EQ(stripKsFilterPrefix(""), std::string(""));
    EXPECT_EQ(stripKsFilterPrefix("USB\\VID_054C"), std::string("USB\\VID_054C"));
    EXPECT_EQ(stripKsFilterPrefix("{1}"), std::string("{1}"));
    EXPECT_EQ(stripKsFilterPrefix("{1}X"), std::string("{1}X"));
    EXPECT_EQ(stripKsFilterPrefix("{}.X"), std::string("{}.X"));
    EXPECT_EQ(stripKsFilterPrefix("{abc}.X"), std::string("{abc}.X"));
    EXPECT_EQ(stripKsFilterPrefix("{1a}.X"), std::string("{1a}.X"));
    EXPECT_EQ(stripKsFilterPrefix("{1.X"), std::string("{1.X"));
    EXPECT_EQ(stripKsFilterPrefix("1}.X"), std::string("1}.X"));
    EXPECT_EQ(stripKsFilterPrefix("{"), std::string("{"));
    EXPECT_EQ(stripKsFilterPrefix("{}"), std::string("{}"));
    EXPECT_EQ(stripKsFilterPrefix(" {1}.X"), std::string(" {1}.X"));

    TEST("an endpoint id is not a KS filter path and must pass through unharmed");
    EXPECT_EQ(stripKsFilterPrefix(SPEAKERS), std::string(SPEAKERS));
}

static void test_decide_restores_a_stolen_default() {
    TEST("current default is ours and the prior default was not: restore it");
    DefaultEndpointDecision d = decideRestore(captured(SPEAKERS), seeing(PAD, true));
    EXPECT(d.action == RestoreAction::Restore);
    EXPECT(d.reason == RestoreReason::Stolen);
    EXPECT_EQ(d.targetId, std::string(SPEAKERS));
}

static void test_decide_never_touches_a_default_that_is_not_ours() {
    TEST("the ONLY trigger is the current default being our endpoint");
    DefaultEndpointDecision d = decideRestore(captured(SPEAKERS), seeing(HEADSET, false));
    EXPECT(d.action == RestoreAction::None);
    EXPECT(d.reason == RestoreReason::NotStolen);
    EXPECT(d.targetId.empty());

    TEST("a user switching to another device mid-window is left alone");
    d = decideRestore(captured(SPEAKERS), seeing(SPEAKERS, false));
    EXPECT(d.action == RestoreAction::None);
    EXPECT(d.reason == RestoreReason::NotStolen);
}

static void test_decide_when_the_current_default_is_unreadable() {
    TEST("no readable current default means no decision at all");
    RoleObservation o;
    o.haveCurrent = false;
    o.currentId = PAD;
    o.currentIsOurs = true;
    DefaultEndpointDecision d = decideRestore(captured(SPEAKERS), o);
    EXPECT(d.action == RestoreAction::None);
    EXPECT(d.reason == RestoreReason::CurrentUnknown);
}

static void test_decide_requires_a_snapshot() {
    TEST("without a captured pre-plug default there is nothing to restore to");
    RoleSnapshot s;
    s.captured = false;
    s.priorId = SPEAKERS;
    DefaultEndpointDecision d = decideRestore(s, seeing(PAD, true));
    EXPECT(d.action == RestoreAction::None);
    EXPECT(d.reason == RestoreReason::NoSnapshot);

    TEST("a default-constructed snapshot is inert");
    d = decideRestore(RoleSnapshot{}, seeing(PAD, true));
    EXPECT(d.action == RestoreAction::None);
    EXPECT(d.reason == RestoreReason::NoSnapshot);
}

static void test_decide_requires_a_non_empty_prior_id() {
    TEST("a captured-but-empty prior default (no render endpoints yet) does nothing");
    DefaultEndpointDecision d = decideRestore(captured(""), seeing(PAD, true));
    EXPECT(d.action == RestoreAction::None);
    EXPECT(d.reason == RestoreReason::NoPriorDefault);

    TEST("whitespace-only is empty too, not an id to hand SetDefaultEndpoint");
    d = decideRestore(captured("   "), seeing(PAD, true));
    EXPECT(d.action == RestoreAction::None);
    EXPECT(d.reason == RestoreReason::NoPriorDefault);
}

static void test_decide_respects_a_deliberate_user_choice() {
    TEST("prior default was already our pad: the user chose it, leave it");
    DefaultEndpointDecision d =
        decideRestore(captured(PAD, /*priorWasOurs=*/true), seeing(PAD, true));
    EXPECT(d.action == RestoreAction::None);
    EXPECT(d.reason == RestoreReason::PriorWasOurs);

    TEST("priorWasOurs wins even when the ids differ (a second pad from a prior run)");
    d = decideRestore(captured(SPEAKERS, /*priorWasOurs=*/true), seeing(PAD, true));
    EXPECT(d.action == RestoreAction::None);
    EXPECT(d.reason == RestoreReason::PriorWasOurs);
}

static void test_decide_will_not_restore_to_the_current_default() {
    TEST("prior id equals the current default: nothing to change");
    DefaultEndpointDecision d = decideRestore(captured(PAD), seeing(PAD, true));
    EXPECT(d.action == RestoreAction::None);
    EXPECT(d.reason == RestoreReason::PriorIsCurrent);

    TEST("the same check is case-insensitive, like every id comparison");
    d = decideRestore(captured("{0.0.0.00000000}.{AAAA}"), seeing("{0.0.0.00000000}.{aaaa}", true));
    EXPECT(d.action == RestoreAction::None);
    EXPECT(d.reason == RestoreReason::PriorIsCurrent);

    TEST("and it tolerates the trailing NUL a REG_SZ read leaves behind");
    d = decideRestore(captured(std::string(PAD) + std::string("\0", 1)), seeing(PAD, true));
    EXPECT(d.action == RestoreAction::None);
    EXPECT(d.reason == RestoreReason::PriorIsCurrent);
}

static void test_decide_is_once_per_role_per_plug() {
    TEST("a role already restored this plug is never restored again");
    DefaultEndpointDecision d = decideRestore(
        captured(SPEAKERS, /*priorWasOurs=*/false, /*restored=*/true), seeing(PAD, true));
    EXPECT(d.action == RestoreAction::None);
    EXPECT(d.reason == RestoreReason::AlreadyRestored);

    TEST("a SECOND promotion after a successful restore is deliberately ignored");
    RoleSnapshot s = captured(SPEAKERS);
    DefaultEndpointDecision first = decideRestore(s, seeing(PAD, true));
    EXPECT(first.action == RestoreAction::Restore);
    noteRestoreResult(s, true);
    DefaultEndpointDecision second = decideRestore(s, seeing(PAD, true));
    EXPECT(second.action == RestoreAction::None);
    EXPECT(second.reason == RestoreReason::AlreadyRestored);
}

static void test_note_restore_result_latches_only_on_success() {
    TEST("a failed SetDefaultEndpoint leaves the latch open for the next poll");
    RoleSnapshot s = captured(SPEAKERS);
    noteRestoreResult(s, false);
    EXPECT(!s.restored);
    DefaultEndpointDecision retry = decideRestore(s, seeing(PAD, true));
    EXPECT(retry.action == RestoreAction::Restore);
    EXPECT_EQ(retry.targetId, std::string(SPEAKERS));

    TEST("a success closes it");
    noteRestoreResult(s, true);
    EXPECT(s.restored);

    TEST("a later failure cannot reopen a closed latch");
    noteRestoreResult(s, false);
    EXPECT(s.restored);
}

static void test_decision_matrix_is_exhaustive() {
    TEST("every combination of the six inputs agrees with the stated rules");
    const char* const priors[] = {"", SPEAKERS, PAD};
    for (int haveCurrent = 0; haveCurrent < 2; ++haveCurrent) {
        for (int currentIsOurs = 0; currentIsOurs < 2; ++currentIsOurs) {
            for (int restored = 0; restored < 2; ++restored) {
                for (int capturedFlag = 0; capturedFlag < 2; ++capturedFlag) {
                    for (int priorWasOurs = 0; priorWasOurs < 2; ++priorWasOurs) {
                        for (const char* prior : priors) {
                            RoleSnapshot s;
                            s.captured = capturedFlag != 0;
                            s.priorId = prior;
                            s.priorWasOurs = priorWasOurs != 0;
                            s.restored = restored != 0;

                            RoleObservation o;
                            o.haveCurrent = haveCurrent != 0;
                            o.currentId = PAD;
                            o.currentIsOurs = currentIsOurs != 0;

                            const bool priorEmpty = std::string(prior).empty();
                            const bool priorIsCurrent = std::string(prior) == std::string(PAD);
                            const bool expect = o.haveCurrent && o.currentIsOurs && !s.restored &&
                                                s.captured && !s.priorWasOurs && !priorEmpty &&
                                                !priorIsCurrent;

                            DefaultEndpointDecision d = decideRestore(s, o);
                            EXPECT_EQ(d.action == RestoreAction::Restore, expect);
                            if (expect) {
                                EXPECT_EQ(d.targetId, std::string(prior));
                                EXPECT(d.reason == RestoreReason::Stolen);
                            } else {
                                EXPECT(d.targetId.empty());
                                EXPECT(d.reason != RestoreReason::Stolen);
                            }
                        }
                    }
                }
            }
        }
    }
}

static void test_reason_names_are_total() {
    TEST("every reason has a log token and the switch has no hole");
    const RestoreReason all[] = {
        RestoreReason::CurrentUnknown, RestoreReason::NotStolen,    RestoreReason::AlreadyRestored,
        RestoreReason::NoSnapshot,     RestoreReason::PriorWasOurs, RestoreReason::NoPriorDefault,
        RestoreReason::PriorIsCurrent, RestoreReason::Stolen,
    };
    for (RestoreReason r : all) {
        const std::string name = restoreReasonName(r);
        EXPECT(!name.empty());
        EXPECT(name != "?");
    }
    EXPECT_EQ(std::string(restoreReasonName(static_cast<RestoreReason>(99))), std::string("?"));
}

static void test_could_restore_later_drives_the_poll_window() {
    TEST("a live snapshot keeps the caller polling");
    EXPECT(couldRestoreLater(captured(SPEAKERS)));

    TEST("every terminal state stops it");
    EXPECT(!couldRestoreLater(RoleSnapshot{}));
    EXPECT(!couldRestoreLater(captured(SPEAKERS, false, /*restored=*/true)));
    EXPECT(!couldRestoreLater(captured(SPEAKERS, /*priorWasOurs=*/true)));
    EXPECT(!couldRestoreLater(captured("")));
    EXPECT(!couldRestoreLater(captured("  \0", false)));
}

static void test_snapshot_roles_are_independent() {
    TEST("roles latch separately: restoring console leaves the others pollable");
    Snapshot snap;
    for (Role r : ALL_ROLES) snap.role(r) = captured(SPEAKERS);
    EXPECT(anyRoleCouldRestoreLater(snap));

    noteRestoreResult(snap.role(Role::Console), true);
    EXPECT(!couldRestoreLater(snap.role(Role::Console)));
    EXPECT(couldRestoreLater(snap.role(Role::Multimedia)));
    EXPECT(couldRestoreLater(snap.role(Role::Communications)));
    EXPECT(anyRoleCouldRestoreLater(snap));

    noteRestoreResult(snap.role(Role::Multimedia), true);
    noteRestoreResult(snap.role(Role::Communications), true);
    EXPECT(!anyRoleCouldRestoreLater(snap));

    TEST("clear() puts it back to inert, so the next plug starts clean");
    snap.clear();
    EXPECT(!anyRoleCouldRestoreLater(snap));
    for (Role r : ALL_ROLES) {
        EXPECT(!snap.role(r).captured);
        EXPECT(!snap.role(r).restored);
        EXPECT(snap.role(r).priorId.empty());
    }

    TEST("a fresh Snapshot is inert without any clear()");
    EXPECT(!anyRoleCouldRestoreLater(Snapshot{}));
}

static void test_plug_lifecycle_walkthrough() {
    TEST("snapshot, three polls, one late promotion: exactly one restore");
    Snapshot snap;
    for (Role r : ALL_ROLES) snap.role(r) = captured(SPEAKERS);

    int restores = 0;
    for (int pass = 0; pass < 4; ++pass) {
        // Promotion is not synchronous with the plug returning; it lands on
        // the third poll here.
        const bool stolen = pass >= 2;
        for (Role r : ALL_ROLES) {
            RoleSnapshot& s = snap.role(r);
            if (!couldRestoreLater(s)) continue;
            DefaultEndpointDecision d = decideRestore(s, seeing(stolen ? PAD : SPEAKERS, stolen));
            if (d.action == RestoreAction::Restore) {
                EXPECT_EQ(d.targetId, std::string(SPEAKERS));
                ++restores;
                noteRestoreResult(s, true);
            }
        }
    }
    EXPECT_EQ(restores, 3);
    EXPECT(!anyRoleCouldRestoreLater(snap));
}

static void test_chain_recognises_our_devnode_by_service() {
    TEST("the usbip2_ude root devnode is proof, whatever the endpoint looks like");
    ChainResult r = classifyParentChain(oursChain());
    EXPECT(r.isOurs());
    EXPECT(r.verdict == ChainVerdict::OursByService);
    EXPECT_EQ(r.hopsExamined, (size_t)3);
    EXPECT(!r.hopCapReached);
    EXPECT(!r.cycleDetected);
}

static void test_chain_recognises_our_devnode_by_hardware_id() {
    TEST("the owner hardware id alone is proof when the service is renamed");
    std::vector<DevNode> chain = oursChain();
    chain.back().service = "usbip2_ude_v3";
    ChainResult r = classifyParentChain(chain);
    EXPECT(r.isOurs());
    EXPECT(r.verdict == ChainVerdict::OursByHardwareId);

    TEST("it matches as a substring of a longer hardware id");
    chain.back().hardwareIds = {"ROOT\\HIDMAESTRO_UDE&REV_0100"};
    EXPECT(classifyParentChain(chain).verdict == ChainVerdict::OursByHardwareId);

    TEST("and case-insensitively, both directions");
    chain.back().hardwareIds = {"root\\hidmaestro_ude"};
    EXPECT(classifyParentChain(chain).verdict == ChainVerdict::OursByHardwareId);
    chain.back().hardwareIds = {"Root\\HidMaestro_Ude"};
    EXPECT(classifyParentChain(chain).verdict == ChainVerdict::OursByHardwareId);
}

static void test_chain_service_match_is_case_insensitive_but_exact() {
    TEST("service matching ignores case");
    std::vector<DevNode> chain = {node("ROOT\\USB\\0000", "USBIP2_UDE")};
    EXPECT(classifyParentChain(chain).verdict == ChainVerdict::OursByService);
    chain[0].service = "  usbip2_ude  ";
    EXPECT(classifyParentChain(chain).verdict == ChainVerdict::OursByService);

    TEST("but it is exact: a lookalike service is NOT ours");
    for (const char* s : {"usbip2_ude2", "xusbip2_ude", "usbip2_ud", "usbip", ""}) {
        chain[0].service = s;
        EXPECT(!classifyParentChain(chain).isOurs());
    }
}

static void test_chain_rejects_a_real_dualsense() {
    TEST("a physical DualSense terminates at PCI and is emphatically not ours");
    ChainResult r = classifyParentChain(realDualSenseChain());
    EXPECT(!r.isOurs());
    EXPECT(r.verdict == ChainVerdict::NotOurs);
    EXPECT_EQ(r.hopsExamined, (size_t)5);
    EXPECT(!r.hopCapReached);
    EXPECT(!r.cycleDetected);

    TEST("its endpoint-level node is byte-identical to ours, so the top hop proves nothing");
    EXPECT_EQ(realDualSenseChain()[0].instanceId, oursChain()[0].instanceId);
    EXPECT(!classifyParentChain({realDualSenseChain()[0]}).isOurs());
}

static void test_chain_rejects_unrelated_hardware() {
    TEST("ordinary endpoints (onboard HD Audio, a USB dock) are not ours");
    std::vector<DevNode> hda = {
        node("HDAUDIO\\FUNC_01&VEN_10DE&DEV_009E\\5&20834512&0&0001", "HDAudBus"),
        node("PCI\\VEN_10DE&DEV_228B\\4&1A2B3C4D&0&0008", "HDAudBus"),
    };
    EXPECT(!classifyParentChain(hda).isOurs());

    std::vector<DevNode> dock = {
        node("USB\\VID_0D8C&PID_0043&MI_00\\C&10D5EFF4&0&0000", "usbaudio"),
        node("USB\\VID_0D8C&PID_0043\\5&1B2C3D4E&0&2", "usbccgp"),
        node("USB\\VID_2109&PID_0822\\6&2C3D4E5F&0&1", "USBHUB3"),
    };
    EXPECT(!classifyParentChain(dock).isOurs());
}

static void test_chain_empty_and_degenerate_inputs() {
    TEST("an empty chain is not ours and reports nothing examined");
    ChainResult r = classifyParentChain({});
    EXPECT(!r.isOurs());
    EXPECT_EQ(r.hopsExamined, (size_t)0);
    EXPECT(!r.hopCapReached);
    EXPECT(!r.cycleDetected);

    TEST("a chain of blank devnodes (every cfgmgr32 read failed) is not ours");
    std::vector<DevNode> blanks = {DevNode{}, DevNode{}, DevNode{}};
    r = classifyParentChain(blanks);
    EXPECT(!r.isOurs());
    EXPECT_EQ(r.hopsExamined, (size_t)3);
    EXPECT(!r.cycleDetected);

    TEST("an empty hardware id in the list cannot match the owner id");
    std::vector<DevNode> chain = {node("ROOT\\USB\\0000", "someservice", {"", "", ""})};
    EXPECT(!classifyParentChain(chain).isOurs());
}

static void test_chain_cycle_guard() {
    TEST("a parent loop stops the walk instead of spinning");
    std::vector<DevNode> loop = {
        node("USB\\A\\1", "usbaudio"),
        node("USB\\B\\1", "usbccgp"),
        node("USB\\A\\1", "usbaudio"),
        node("ROOT\\USB\\0000", "usbip2_ude"),
    };
    ChainResult r = classifyParentChain(loop);
    EXPECT(r.cycleDetected);
    EXPECT(!r.isOurs());
    EXPECT_EQ(r.hopsExamined, (size_t)2);

    TEST("the visited check is case-insensitive, like every id comparison");
    loop[2].instanceId = "usb\\a\\1";
    EXPECT(classifyParentChain(loop).cycleDetected);

    TEST("a self-loop trips on the second hop");
    std::vector<DevNode> self = {node("USB\\A\\1", "x"), node("USB\\A\\1", "x")};
    r = classifyParentChain(self);
    EXPECT(r.cycleDetected);
    EXPECT_EQ(r.hopsExamined, (size_t)1);

    TEST("a match already found is reported even if the chain would loop after it");
    std::vector<DevNode> matchThenLoop = {
        node("ROOT\\USB\\0000", "usbip2_ude"),
        node("ROOT\\USB\\0000", "usbip2_ude"),
    };
    r = classifyParentChain(matchThenLoop);
    EXPECT(r.verdict == ChainVerdict::OursByService);
    EXPECT(!r.cycleDetected);

    TEST("blank instance ids are untrackable and must not fake a cycle");
    std::vector<DevNode> blanks = {DevNode{}, DevNode{}, node("ROOT\\USB\\0000", "usbip2_ude")};
    r = classifyParentChain(blanks);
    EXPECT(!r.cycleDetected);
    EXPECT(r.verdict == ChainVerdict::OursByService);
}

static void test_chain_hop_cap() {
    TEST("a chain longer than the cap stops at the cap without a verdict");
    std::vector<DevNode> deep;
    for (int i = 0; i < 20; ++i) deep.push_back(node("USB\\N\\" + std::to_string(i), "usbhub3"));
    ChainResult r = classifyParentChain(deep);
    EXPECT(!r.isOurs());
    EXPECT(r.hopCapReached);
    EXPECT_EQ(r.hopsExamined, MAX_PARENT_HOPS);
    EXPECT(!r.cycleDetected);

    TEST("our devnode just past the cap is missed rather than searched forever");
    deep.resize(MAX_PARENT_HOPS);
    deep.push_back(node("ROOT\\USB\\0000", "usbip2_ude"));
    EXPECT(!classifyParentChain(deep).isOurs());

    TEST("our devnode on the last allowed hop is still found");
    std::vector<DevNode> justInside;
    for (size_t i = 0; i + 1 < MAX_PARENT_HOPS; ++i)
        justInside.push_back(node("USB\\N\\" + std::to_string(i), "usbhub3"));
    justInside.push_back(node("ROOT\\USB\\0000", "usbip2_ude"));
    r = classifyParentChain(justInside);
    EXPECT(r.verdict == ChainVerdict::OursByService);
    EXPECT_EQ(r.hopsExamined, MAX_PARENT_HOPS);
    EXPECT(!r.hopCapReached);

    TEST("an explicit cap is honoured");
    r = classifyParentChain(realDualSenseChain(), 2);
    EXPECT(!r.isOurs());
    EXPECT(r.hopCapReached);
    EXPECT_EQ(r.hopsExamined, (size_t)2);

    r = classifyParentChain(oursChain(), 3);
    EXPECT(r.verdict == ChainVerdict::OursByService);
    r = classifyParentChain(oursChain(), 2);
    EXPECT(!r.isOurs());
    EXPECT(r.hopCapReached);

    TEST("a zero cap examines nothing and claims nothing");
    r = classifyParentChain(oursChain(), 0);
    EXPECT(!r.isOurs());
    EXPECT(r.hopCapReached);
    EXPECT_EQ(r.hopsExamined, (size_t)0);
}

static void test_chain_scanner_stops_the_io_walk_early() {
    TEST("accept() returns false the moment the answer is known");
    ChainScanner s;
    EXPECT(s.accept(node("USB\\VID_054C&PID_0CE6&MI_00\\3&25", "usbaudio")));
    EXPECT(s.accept(node("USB\\VID_054C&PID_0CE6\\SAT1", "usbccgp")));
    EXPECT(!s.accept(node("ROOT\\USB\\0000", "usbip2_ude")));
    EXPECT(s.result().verdict == ChainVerdict::OursByService);
    EXPECT_EQ(s.result().hopsExamined, (size_t)2 + 1);

    TEST("further nodes after a verdict are refused and change nothing");
    EXPECT(!s.accept(node("PCI\\VEN_1022", "USBXHCI")));
    EXPECT_EQ(s.result().hopsExamined, (size_t)3);
    EXPECT(s.result().verdict == ChainVerdict::OursByService);

    TEST("a fresh scanner reports the inert result before any node");
    ChainScanner fresh;
    EXPECT(!fresh.result().isOurs());
    EXPECT_EQ(fresh.result().hopsExamined, (size_t)0);

    TEST("a scanner past the cap keeps refusing");
    ChainScanner tiny(1);
    EXPECT(!tiny.accept(node("USB\\A\\1", "usbhub3")));
    EXPECT(tiny.result().hopCapReached);
    EXPECT(!tiny.accept(node("ROOT\\USB\\0000", "usbip2_ude")));
    EXPECT(!tiny.result().isOurs());
    EXPECT_EQ(tiny.result().hopsExamined, (size_t)1);

    TEST("a scanner that hit a cycle keeps refusing");
    ChainScanner cyc;
    EXPECT(cyc.accept(node("USB\\A\\1", "x")));
    EXPECT(!cyc.accept(node("USB\\A\\1", "x")));
    EXPECT(!cyc.accept(node("ROOT\\USB\\0000", "usbip2_ude")));
    EXPECT(cyc.result().cycleDetected);
    EXPECT(!cyc.result().isOurs());
}

static void test_chain_verdict_names_are_total() {
    TEST("every verdict has a log token");
    EXPECT_EQ(std::string(chainVerdictName(ChainVerdict::NotOurs)), std::string("not-ours"));
    EXPECT_EQ(std::string(chainVerdictName(ChainVerdict::OursByService)),
              std::string("ours-by-service"));
    EXPECT_EQ(std::string(chainVerdictName(ChainVerdict::OursByHardwareId)),
              std::string("ours-by-hardware-id"));
    EXPECT_EQ(std::string(chainVerdictName(static_cast<ChainVerdict>(9))), std::string("?"));
}

static void test_policy_config_slots_are_pinned() {
    TEST("the vtable slots are the contract with AudioSes.dll, so pin them");
    EXPECT_EQ(POLICY_CONFIG_SLOT_GET_PROCESSING_PERIOD, 7);
    EXPECT_EQ(POLICY_CONFIG_SLOT_SET_DEFAULT_ENDPOINT, 13);
}

static void test_probe_accepts_the_verified_layout() {
    TEST("CoCreate ok, S_OK, and two sane periods: slot 13 is safe to call");
    EXPECT(evaluatePolicyConfigProbe(goodProbe()) == ProbeVerdict::Usable);
    EXPECT(policyConfigSlotIsSafe(goodProbe()));

    TEST("equal periods are plausible (min == default)");
    PolicyConfigProbe p = goodProbe();
    p.minPeriod = p.defaultPeriod;
    EXPECT(policyConfigSlotIsSafe(p));

    TEST("a single-hundred-nanosecond period is still positive and ordered");
    p = goodProbe();
    p.defaultPeriod = 1;
    p.minPeriod = 1;
    EXPECT(policyConfigSlotIsSafe(p));
}

static void test_probe_rejects_a_failed_cocreate() {
    TEST("no object means no call, whatever the other fields say");
    PolicyConfigProbe p = goodProbe();
    p.coCreateOk = false;
    EXPECT(evaluatePolicyConfigProbe(p) == ProbeVerdict::CoCreateFailed);
    EXPECT(!policyConfigSlotIsSafe(p));

    TEST("a default-constructed probe is rejected");
    EXPECT(evaluatePolicyConfigProbe(PolicyConfigProbe{}) == ProbeVerdict::CoCreateFailed);
}

static void test_probe_rejects_a_failing_hresult() {
    TEST("the Vista layout answers slot 7 with ERROR_NOT_SUPPORTED; that must disable us");
    PolicyConfigProbe p = goodProbe();
    p.probeHr = static_cast<int32_t>(0x80070032u);
    EXPECT(evaluatePolicyConfigProbe(p) == ProbeVerdict::ProbeCallFailed);

    TEST("so must every other failure code the probe has been seen to return");
    for (uint32_t hr : {0x80070490u, 0x800706F4u, 0x80004003u, 0x80070057u, 0x80004002u}) {
        p = goodProbe();
        p.probeHr = static_cast<int32_t>(hr);
        EXPECT(evaluatePolicyConfigProbe(p) == ProbeVerdict::ProbeCallFailed);
    }

    TEST("SUCCEEDED but not S_OK is an unrecognised layout, so it is refused too");
    p = goodProbe();
    p.probeHr = 1; // S_FALSE
    EXPECT(evaluatePolicyConfigProbe(p) == ProbeVerdict::ProbeCallFailed);

    TEST("the HRESULT is checked before the periods, so garbage out-params do not mislead");
    p = goodProbe();
    p.probeHr = static_cast<int32_t>(0x80070032u);
    p.defaultPeriod = 0;
    p.minPeriod = 0;
    EXPECT(evaluatePolicyConfigProbe(p) == ProbeVerdict::ProbeCallFailed);
}

static void test_probe_rejects_implausible_periods() {
    TEST("S_OK with untouched out-params is not proof of anything");
    PolicyConfigProbe p = goodProbe();
    p.defaultPeriod = 0;
    p.minPeriod = 0;
    EXPECT(evaluatePolicyConfigProbe(p) == ProbeVerdict::ImplausiblePeriods);

    TEST("either period alone being zero is enough to refuse");
    p = goodProbe();
    p.defaultPeriod = 0;
    EXPECT(evaluatePolicyConfigProbe(p) == ProbeVerdict::ImplausiblePeriods);
    p = goodProbe();
    p.minPeriod = 0;
    EXPECT(evaluatePolicyConfigProbe(p) == ProbeVerdict::ImplausiblePeriods);

    TEST("negatives are refused (a shifted vtable would write nonsense here)");
    p = goodProbe();
    p.defaultPeriod = -1;
    EXPECT(evaluatePolicyConfigProbe(p) == ProbeVerdict::ImplausiblePeriods);
    p = goodProbe();
    p.minPeriod = -100000;
    EXPECT(evaluatePolicyConfigProbe(p) == ProbeVerdict::ImplausiblePeriods);

    TEST("min above default means the two out-params are not what we think they are");
    p = goodProbe();
    p.minPeriod = p.defaultPeriod + 1;
    EXPECT(evaluatePolicyConfigProbe(p) == ProbeVerdict::ImplausiblePeriods);
    EXPECT(!policyConfigSlotIsSafe(p));
}

static void test_probe_verdict_names_are_total() {
    TEST("every verdict has a log token");
    const ProbeVerdict all[] = {ProbeVerdict::Usable, ProbeVerdict::CoCreateFailed,
                                ProbeVerdict::ProbeCallFailed, ProbeVerdict::ImplausiblePeriods};
    for (ProbeVerdict v : all) {
        const std::string name = probeVerdictName(v);
        EXPECT(!name.empty());
        EXPECT(name != "?");
    }
    EXPECT_EQ(std::string(probeVerdictName(static_cast<ProbeVerdict>(7))), std::string("?"));
}

int main() {
    test_roles_match_the_erole_enum();
    test_normalize_device_id();
    test_device_ids_equal_is_case_insensitive();
    test_device_id_contains();
    test_strip_ks_filter_prefix();

    test_decide_restores_a_stolen_default();
    test_decide_never_touches_a_default_that_is_not_ours();
    test_decide_when_the_current_default_is_unreadable();
    test_decide_requires_a_snapshot();
    test_decide_requires_a_non_empty_prior_id();
    test_decide_respects_a_deliberate_user_choice();
    test_decide_will_not_restore_to_the_current_default();
    test_decide_is_once_per_role_per_plug();
    test_note_restore_result_latches_only_on_success();
    test_decision_matrix_is_exhaustive();
    test_reason_names_are_total();
    test_could_restore_later_drives_the_poll_window();
    test_snapshot_roles_are_independent();
    test_plug_lifecycle_walkthrough();

    test_chain_recognises_our_devnode_by_service();
    test_chain_recognises_our_devnode_by_hardware_id();
    test_chain_service_match_is_case_insensitive_but_exact();
    test_chain_rejects_a_real_dualsense();
    test_chain_rejects_unrelated_hardware();
    test_chain_empty_and_degenerate_inputs();
    test_chain_cycle_guard();
    test_chain_hop_cap();
    test_chain_scanner_stops_the_io_walk_early();
    test_chain_verdict_names_are_total();

    test_policy_config_slots_are_pinned();
    test_probe_accepts_the_verified_layout();
    test_probe_rejects_a_failed_cocreate();
    test_probe_rejects_a_failing_hresult();
    test_probe_rejects_implausible_periods();
    test_probe_verdict_names_are_total();

    std::cout << "audio_default_guard: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
