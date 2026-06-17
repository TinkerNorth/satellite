// SPDX-License-Identifier: LGPL-3.0-or-later

// Boundary to the elevated, infrequent HIDMaestro device lifecycle. The future
// NativeAOT C# provisioner (deferred — see PR notes) implements this: install
// the UMDF2 driver, synthesize the HID descriptor for a controller type, create
// the SwDevice node, and create the named shared-memory sections with the
// LocalService SDDL (WUDFHost lacks SeCreateGlobalPrivilege, so it can't create
// the Global\ sections itself). The native hot path then writes the returned
// input section directly (hidmaestro_wire.h) with no managed code per frame.
#pragma once

#include <cstdint>
#include <string>

namespace satellite {
namespace hidmaestro {

// What the provisioner hands back so the native hot path can open and write the
// per-controller sections (input layout: hidmaestro_wire.h).
struct ProvisionResult {
    std::string inputSectionName;  // e.g. Global\HIDMaestroInput<N>
    std::string outputSectionName; // e.g. Global\HIDMaestroOutput<N> (rumble/FFB ring)
    uint32_t controllerIndex = 0;
};

class IHidMaestroProvisioner {
  public:
    virtual ~IHidMaestroProvisioner() = default;

    // Create (or reuse) a virtual controller of `controllerType` (CONTROLLER_TYPE_*)
    // bound to `serial`. Populates `out` on success.
    virtual bool provision(uint32_t serial, uint8_t controllerType, ProvisionResult& out) = 0;

    // Tear down the controller for `serial`. Idempotent.
    virtual bool deprovision(uint32_t serial) = 0;
};

} // namespace hidmaestro
} // namespace satellite
