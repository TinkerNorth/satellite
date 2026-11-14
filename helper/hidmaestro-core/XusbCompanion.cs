// SPDX-License-Identifier: LGPL-3.0-or-later
namespace Satellite.HmHelper.Core;

// Whether the SDK gives a profile an XUSB companion, read off the profile's
// own xinput block: its "via" is XInputSpec.ViaXusbCompanion, which the SDK
// compares ordinally (XInputPresentation.cs:25), and the profile runs on the
// UMDF2 driver, since the USB/IP create path makes none
// (PresentationPlan.cs:93-94). A profile can also take its presentation from a
// device model, which the public HMProfile.XInput does not show, so check-sdk
// holds this to the SDK's own resolution for every profile Satellite plugs.
public static class XusbCompanion
{
    public const string Via = "xusbCompanion";

    public static bool Presents(string? via, bool usbip) =>
        !usbip && string.Equals(via, Via, StringComparison.Ordinal);
}
