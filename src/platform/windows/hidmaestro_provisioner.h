// SPDX-License-Identifier: LGPL-3.0-or-later

// Boundary to the elevated, infrequent HIDMaestro device lifecycle. The
// production implementation (hidmaestro_helper_client.cpp) drives the bundled
// .NET helper process, which uses the HIDMaestro SDK to install the UMDF2
// driver and create/destroy the SwDevice nodes, creates the Global\ shared
// sections (WUDFHost runs as LocalService without SeCreateGlobalPrivilege, and
// a non-elevated satellite can only open them read-only), and duplicates the
// section/event handles into this process. The native hot path then writes
// the duplicated input section directly (hidmaestro_wire.h) with no managed
// code and no elevation per frame. Handles are uint64 so this interface stays
// <windows.h>-free and the adapter tests can fake it with in-process objects.
#pragma once

#include "core/types.h"

#include <cstdint>

namespace satellite {
namespace hidmaestro {

// Duplicated handles owned by the caller (CloseHandle when done). Zero =
// absent: companionEvent only exists for Xbox-VID profiles, outputSection/
// outputEvent are best-effort (rumble return path degrades to none).
struct ProvisionResult {
    uint32_t controllerIndex = 0;
    uint64_t inputSection = 0;
    uint64_t inputEvent = 0;
    uint64_t companionEvent = 0;
    uint64_t outputSection = 0;
    uint64_t outputEvent = 0;

    // Controller-audio rings (hidmaestro_audio_wire.h), present only for a
    // composite persona that actually materialized its USB-audio function.
    // Zero on every other profile, and on a composite whose helper could not
    // create the sections: audio degrades to absent, the pad still works.
    uint64_t speakerSection = 0;
    uint64_t speakerEvent = 0;
    uint64_t micSection = 0;
    uint64_t micEvent = 0;

    // The persona's endpoint formats, read off the SDK at plug time and
    // constant for the life of the plug, so they ride here instead of in every
    // ring slot. Zero = that direction has no endpoint. They are NOT assumed to
    // be the wire format: the DualShock 4 v2 persona is 32 kHz out / 16 kHz in.
    int speakerChannels = 0;
    int speakerRateHz = 0;
    int micChannels = 0;
    int micRateHz = 0;
};

class IHidMaestroProvisioner {
  public:
    virtual ~IHidMaestroProvisioner() = default;

    // Cheap, prompt-free installed-ness probe (driver store / helper binary).
    virtual bool installed() const = 0;

    // True once the elevated lifecycle channel is up. ensureReady() may show
    // a UAC prompt, so callers keep it off any status/probe path and invoke
    // it only when a controller is actually being plugged.
    virtual bool isReady() const = 0;
    virtual bool ensureReady() = 0;
    virtual void shutdown() = 0;

    // Create a virtual controller of `identity` on `serial`. Populates `out`
    // on success; on failure the serial holds no device. `audio` asks for the
    // identity's composite persona (see profileForIdentity): it can fail on
    // its own, because materializing one installs HIDMaestro's bundled kernel
    // USB transport, so the caller is expected to retry without it.
    virtual bool provision(uint32_t serial, GamepadIdentity identity, bool audio,
                           ProvisionResult& out) = 0;

    // Tear down the controller for `serial`. False = removal unconfirmed (the
    // caller must quarantine the serial). Idempotent.
    virtual bool deprovision(uint32_t serial) = 0;
};

} // namespace hidmaestro
} // namespace satellite
