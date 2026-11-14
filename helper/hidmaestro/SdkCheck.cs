// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Reflection;
using HIDMaestro;
using HIDMaestro.Internal;
using Satellite.HmHelper.Core;

namespace Satellite.HmHelper;

// Holds what Satellite restates of the SDK's internals to the SDK the helper was
// built with: the identity tag IdentityTags derives (DeviceIdentity is
// internal); the SDK version SharedMemoryIO writes into every version section,
// which driver_pins.h pins for satellite.exe's check of that section; that
// section's size, offsets, layout and protocols as src/core/hidmaestro_version.h
// and VersionSection state them; the XUSB companion the helper reads off each
// profile Satellite plugs; and the timeout scale the helper reports.
// verify-helper-sdk.ps1 runs it on the published helper. A reflection target
// the SDK no longer has fails it, so an SDK that renames one stops the publish.
internal static class SdkCheck
{
    private const string IdentityTypeName = "HIDMaestro.Internal.DeviceIdentity";
    private const string SharedMemoryTypeName = "HIDMaestro.Internal.SharedMemoryIO";
    private const BindingFlags StaticMembers = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static;
    private static readonly uint[] Serials = { 1, 2, 9, 10, 16 };
    private static readonly string[] UpstreamTokens = { "HM_0000", "HM_0123456789ABCDEF" };
    private static readonly string[] PluggedProfiles = SatelliteProfiles.Copies.Append("xbox-360-wired").ToArray();

    // Each SDK constant of the version contract (SharedMemoryIO.cs:195-216), the
    // name hidmaestro_version.h gives it, and VersionSection's own where the
    // helper reads the section itself.
    private static readonly (string Sdk, string Header, int? Core)[] VersionConstants =
    {
        ("VERSION_INFO_SIZE", "VERSION_SECTION_SIZE", VersionSection.Size),
        ("VERSION_LAYOUT", "VERSION_LAYOUT", null),
        ("SDK_PROTOCOL_MIN", "SDK_PROTOCOL_MIN", null),
        ("SDK_PROTOCOL", "SDK_PROTOCOL", null),
        ("VERSION_OFFSET_LAYOUT", "VERSION_LAYOUT_OFFSET", VersionSection.LayoutOffset),
        ("VERSION_OFFSET_SDK_PROTOCOL_MIN", "VERSION_SDK_PROTOCOL_MIN_OFFSET", VersionSection.SdkProtocolMinOffset),
        ("VERSION_OFFSET_SDK_PROTOCOL_MAX", "VERSION_SDK_PROTOCOL_MAX_OFFSET", VersionSection.SdkProtocolMaxOffset),
        ("VERSION_OFFSET_SDK_VERSION", "VERSION_SDK_VERSION_OFFSET", VersionSection.SdkVersionOffset),
        ("VERSION_OFFSET_DRIVER_PROTOCOL", "VERSION_DRIVER_PROTOCOL_OFFSET", VersionSection.DriverProtocolOffset),
        ("VERSION_OFFSET_DRIVER_VERSION", "VERSION_DRIVER_VERSION_OFFSET", VersionSection.DriverVersionOffset),
        ("VERSION_OFFSET_DRIVER_CAPS", "VERSION_DRIVER_CAPABILITIES_OFFSET", VersionSection.DriverCapabilitiesOffset),
        ("VERSION_OFFSET_DRIVER_PROTOCOL_MIN", "VERSION_DRIVER_PROTOCOL_MIN_OFFSET", VersionSection.DriverProtocolMinOffset),
        ("VERSION_OFFSET_COMPANION_PROTOCOL", "VERSION_COMPANION_PROTOCOL_OFFSET", VersionSection.CompanionProtocolOffset),
        ("VERSION_OFFSET_COMPANION_PROTOCOL_MIN", "VERSION_COMPANION_PROTOCOL_MIN_OFFSET", VersionSection.CompanionProtocolMinOffset),
        ("VERSION_OFFSET_IDENTITY_TAG", "VERSION_IDENTITY_TAG_OFFSET", VersionSection.IdentityTagOffset),
        ("VERSION_OFFSET_DRIVER_CONFIG_ERROR", "VERSION_DRIVER_CONFIG_ERROR_OFFSET", VersionSection.DriverConfigErrorOffset),
        ("VERSION_OFFSET_COMPANION_CONFIG_ERROR", "VERSION_COMPANION_CONFIG_ERROR_OFFSET", VersionSection.CompanionConfigErrorOffset),
    };

    public static int Run(string sdkPin, string versionHeader)
    {
        int failures = 0;
        void Check(bool ok, string what)
        {
            Console.WriteLine((ok ? "PASS " : "FAIL ") + what);
            if (!ok) failures++;
        }

        var sdk = new SdkIdentity();
        foreach (string key in Keys())
        {
            (string token, ulong tag) = sdk.ForKey(key);
            Check(IdentityTags.TokenOf(key) == token, $"'{key}': IdentityTags derives {IdentityTags.TokenOf(key)}, the SDK {token}");
            Check(IdentityTags.OfKey(key) == tag, $"'{key}': IdentityTags tags it {IdentityTags.OfKey(key):X16}, the SDK {tag:X16}");
        }
        foreach (string token in UpstreamTokens)
            Check(IdentityTags.OfToken(token) == sdk.TagOf(token), $"{token}: IdentityTags tags it {IdentityTags.OfToken(token):X16}, the SDK {sdk.TagOf(token):X16}");

        Version written = WrittenSdkVersion();
        Version pinned = Release(Version.Parse(sdkPin));
        Check(written == pinned, $"the SDK writes SDK version {written} into a version section, its {SharedMemoryTypeName}'s assembly version, and driver_pins.h pins {pinned}");

        CheckVersionConstants(CppConstants.Parse(File.ReadAllText(versionHeader)), versionHeader, Check);
        CheckCompanions(Check);

        double scale = SdkTimeouts.Scale;
        Check(scale >= 0.1 && scale <= 100.0, $"the helper reads the SDK's {SdkTimeouts.TypeName}.{SdkTimeouts.FieldName}, {scale} in this process");

        Console.WriteLine(failures == 0 ? "check-sdk: all passed" : $"check-sdk: {failures} failed");
        return failures == 0 ? 0 : 1;
    }

    // Every identity key shape the helper creates controllers with: each profile
    // satellite.exe asks for, at one- and two-digit serials, and a key the SDK
    // has to trim.
    private static IEnumerable<string> Keys()
    {
        foreach (uint serial in Serials)
        {
            foreach (string profile in PluggedProfiles) yield return SatelliteProfiles.IdentityKey(serial, profile);
        }
        yield return "  " + SatelliteProfiles.IdentityKey(3, "dualsense") + "\t";
    }

    // satellite.exe judges a version section with the header's values and the
    // helper's selftest reads one with VersionSection's, so each is the SDK's,
    // and every field the SDK lays out is one both read.
    private static void CheckVersionConstants(IReadOnlyDictionary<string, long> header, string headerPath,
                                              Action<bool, string> check)
    {
        Type shared = SdkType(SharedMemoryTypeName);
        foreach ((string sdkName, string headerName, int? core) in VersionConstants)
        {
            int sdk = IntConstant(shared, sdkName);
            bool stated = header.TryGetValue(headerName, out long stating);
            check(stated && stating == sdk, stated
                ? $"{headerName} is {stating} in {headerPath}, the SDK's {sdkName} {sdk}"
                : $"{headerPath} states no {headerName}, the SDK's {sdkName} {sdk}");
            if (core is int value)
                check(value == sdk, $"VersionSection reads {sdkName} as {value}, the SDK {sdk}");
        }
        foreach (FieldInfo field in shared.GetFields(StaticMembers).Where(f => f.IsLiteral && f.Name.StartsWith("VERSION_OFFSET_", StringComparison.Ordinal)))
            check(VersionConstants.Any(c => c.Sdk == field.Name), $"the SDK's {field.Name} is a field Satellite reads");
    }

    // The companion the helper reports at plug, read off each profile's own
    // xinput block as XusbCompanion reads it, against the SDK's resolution of
    // the profile's presentation (ControllerProfile.RequiresXusbCompanion, which
    // its create plan follows on the UMDF2 path) for the catalog profile each of
    // Satellite's profiles is or copies; check-profiles holds every copy's xinput
    // block to its catalog profile's.
    private static void CheckCompanions(Action<bool, string> check)
    {
        FieldInfo via = typeof(XInputSpec).GetField("ViaXusbCompanion", StaticMembers)
            ?? throw new InvalidOperationException("the pinned SDK's XInputSpec has no ViaXusbCompanion");
        check(via.IsLiteral && via.GetRawConstantValue() as string == XusbCompanion.Via,
              $"XusbCompanion reads via '{XusbCompanion.Via}', the SDK's XInputSpec.ViaXusbCompanion '{(via.IsLiteral ? via.GetRawConstantValue() : null)}'");
        ProfileDatabase catalog = ProfileDatabase.LoadEmbedded();
        foreach (string id in PluggedProfiles)
        {
            ControllerProfile? profile = catalog.All.FirstOrDefault(p => p.Id == id);
            if (profile == null)
            {
                check(false, $"{id}: the pinned SDK's catalog has no such profile");
                continue;
            }
            bool reported = XusbCompanion.Presents(profile.XInput?.Via, profile.RequiresUsbipBackend);
            bool created = profile.RequiresXusbCompanion && !profile.RequiresUsbipBackend;
            check(reported == created,
                  $"{id}: the helper reports {(reported ? "an XUSB companion" : "no XUSB companion")}, and the SDK creates {(created ? "one" : "none")}");
        }
    }

    private static int IntConstant(Type type, string name)
    {
        FieldInfo field = type.GetField(name, StaticMembers)
            ?? throw new InvalidOperationException($"the pinned SDK's {type.FullName} has no {name}");
        return field.IsLiteral && field.GetRawConstantValue() is int value
            ? value
            : throw new InvalidOperationException($"{type.FullName}.{name} is no longer an int constant");
    }

    // SharedMemoryIO.EnsureInputMapping writes Major, Minor and max(0, Build) of
    // typeof(SharedMemoryIO).Assembly.GetName().Version into a section's SDK half.
    private static Version WrittenSdkVersion()
    {
        Version assembly = SdkType(SharedMemoryTypeName).Assembly.GetName().Version
            ?? throw new InvalidOperationException($"the pinned SDK's {SharedMemoryTypeName} assembly has no version");
        return Release(assembly);
    }

    private static Version Release(Version version) => new(version.Major, version.Minor, Math.Max(0, version.Build));

    private static Type SdkType(string name) =>
        typeof(HMContext).Assembly.GetType(name, throwOnError: false)
        ?? throw new InvalidOperationException($"the pinned SDK has no {name}; re-derive what Satellite restates of it for this SDK");

    private sealed class SdkIdentity
    {
        private const BindingFlags InstanceMembers = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance;

        private readonly MethodInfo _forKey;
        private readonly MethodInfo _tagOf;
        private readonly PropertyInfo _token;
        private readonly PropertyInfo _tag;

        public SdkIdentity()
        {
            Type type = SdkType(IdentityTypeName);
            _forKey = Method(type, "ForKey", typeof(string), typeof(int));
            _tagOf = Method(type, "TagOf", typeof(string));
            _token = Property(type, "Token");
            _tag = Property(type, "Tag");
        }

        public (string Token, ulong Tag) ForKey(string key)
        {
            object? identity = _forKey.Invoke(null, new object[] { key, 0 });
            if (identity == null) throw new InvalidOperationException($"{IdentityTypeName}.ForKey returned no identity for '{key}'");
            if (_token.GetValue(identity) is not string token || _tag.GetValue(identity) is not ulong tag)
                throw new InvalidOperationException($"{IdentityTypeName}.Token and .Tag are no longer a string and a ulong");
            return (token, tag);
        }

        public ulong TagOf(string token) =>
            _tagOf.Invoke(null, new object[] { token }) is ulong tag
                ? tag
                : throw new InvalidOperationException($"{IdentityTypeName}.TagOf no longer returns a ulong");

        private static MethodInfo Method(Type type, string name, params Type[] parameters) =>
            type.GetMethod(name, StaticMembers, null, parameters, null)
            ?? throw new InvalidOperationException($"the pinned SDK's {type.FullName} has no static {name}({string.Join(", ", parameters.Select(p => p.Name))})");

        private static PropertyInfo Property(Type type, string name) =>
            type.GetProperty(name, InstanceMembers)
            ?? throw new InvalidOperationException($"the pinned SDK's {type.FullName} has no {name}");
    }
}
