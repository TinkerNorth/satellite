// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Globalization;

namespace Satellite.HmHelper.Core;

// HIDMAESTRO_TIMEOUT_SCALE as the SDK resolves it (TimeoutScale.ResolveFactor,
// TimeoutScale.cs:64-90 at v2.1.0): a blank value, one that is not an
// invariant-culture number, or a number outside [0.1, 100.0] leaves the SDK's
// waits unscaled. The SDK reads it once from its own process's environment,
// which is the helper's, so the helper reports it for satellite.exe to stretch
// its own wait for a driver half by. check-sdk holds this to the SDK's
// TimeoutScale.Factor under several values.
public static class SdkTimeoutScale
{
    public const string Variable = "HIDMAESTRO_TIMEOUT_SCALE";
    public const double Unscaled = 1.0;
    public const double Smallest = 0.1;
    public const double Largest = 100.0;

    public static double Resolve(string? raw)
    {
        if (string.IsNullOrWhiteSpace(raw)) return Unscaled;
        if (!double.TryParse(raw, NumberStyles.Float, CultureInfo.InvariantCulture, out double factor)) return Unscaled;
        if (factor < Smallest || factor > Largest) return Unscaled;
        return factor;
    }
}
