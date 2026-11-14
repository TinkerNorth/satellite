// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Reflection;
using HIDMaestro;
using Satellite.HmHelper.Core;

namespace Satellite.HmHelper;

// Holds what Satellite restates of the SDK's internals to the SDK the helper was
// built with: the identity tag IdentityTags derives (DeviceIdentity is
// internal), and the SDK version SharedMemoryIO writes into every version
// section, which driver_pins.h pins for satellite.exe's check of that section.
// verify-helper-sdk.ps1 runs it on the published helper. A reflection target
// the SDK no longer has fails it, so an SDK that renames one stops the publish.
internal static class SdkCheck
{
    private const string IdentityTypeName = "HIDMaestro.Internal.DeviceIdentity";
    private const string SharedMemoryTypeName = "HIDMaestro.Internal.SharedMemoryIO";
    private static readonly uint[] Serials = { 1, 2, 9, 10, 16 };
    private static readonly string[] UpstreamTokens = { "HM_0000", "HM_0123456789ABCDEF" };

    public static int Run(string sdkPin)
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

        Console.WriteLine(failures == 0 ? "check-sdk: all passed" : $"check-sdk: {failures} failed");
        return failures == 0 ? 0 : 1;
    }

    // Every identity key shape the helper creates controllers with: each profile
    // satellite.exe asks for, at one- and two-digit serials, and a key the SDK
    // has to trim.
    private static IEnumerable<string> Keys()
    {
        IEnumerable<string> profiles = SatelliteProfiles.Copies.Append("xbox-360-wired");
        foreach (uint serial in Serials)
        {
            foreach (string profile in profiles) yield return SatelliteProfiles.IdentityKey(serial, profile);
        }
        yield return "  " + SatelliteProfiles.IdentityKey(3, "dualsense") + "\t";
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
        private const BindingFlags StaticMembers = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static;
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
