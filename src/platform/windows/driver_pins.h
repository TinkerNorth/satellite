// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#define SATELLITE_VIGEMBUS_BUNDLED_VERSION "1.22.0"
#define SATELLITE_VIGEMBUS_BUNDLED_DRIVER_VERSION "1.21.442.0"
#define SATELLITE_HIDMAESTRO_SDK_VERSION "2.1.0"
#define SATELLITE_HIDMAESTRO_BUNDLED_DRIVER_VERSION "2.1.0.1716"

// The HIDMaestro driver releases Satellite supports, as the version section
// names them (major.minor.patch), comma-separated. A pad on one of them is
// written without comment; a release not listed that still serves protocol 4
// is written too, logged once per change as unverified. A release joins once
// its driver diff from a listed release has been reviewed for anything that
// touches the input or output sections, their seqlock, their events, SeqNo or
// the signal gate, and `satellite-hm-helper.exe selftest` has passed on
// hardware running it. The bundled driver's release must be listed
// (test_hidmaestro_version, and check-sdk against the SDK's own INF).
#define SATELLITE_HIDMAESTRO_VERIFIED_DRIVER_RELEASES "2.1.0"
