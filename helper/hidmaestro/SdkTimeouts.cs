// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Reflection;
using HIDMaestro;

namespace Satellite.HmHelper;

// HIDMAESTRO_TIMEOUT_SCALE as the SDK in this process resolved it
// (TimeoutScale.Factor, internal, read once from this process's environment
// and clamped to [0.1, 100], else 1.0). The SDK stretches its own start waits
// by it, so satellite.exe stretches its wait for a driver half by the same
// factor. The helper may run as a service whose environment satellite.exe does
// not share, hence the report. check-sdk fails when the pinned SDK no longer has
// the field, so the publish stops before a plug could.
internal static class SdkTimeouts
{
    public const string TypeName = "HIDMaestro.Internal.TimeoutScale";
    public const string FieldName = "Factor";

    private static readonly Lazy<double> Resolved = new(Read);

    public static double Scale => Resolved.Value;

    private static double Read()
    {
        FieldInfo field = typeof(HMContext).Assembly.GetType(TypeName, throwOnError: false)
                ?.GetField(FieldName, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)
            ?? throw new InvalidOperationException($"the pinned SDK has no {TypeName}.{FieldName}");
        return field.GetValue(null) is double factor
            ? factor
            : throw new InvalidOperationException($"{TypeName}.{FieldName} is no longer a double");
    }
}
