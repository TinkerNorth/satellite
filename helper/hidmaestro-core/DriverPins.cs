// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Globalization;
using System.Text.RegularExpressions;

namespace Satellite.HmHelper.Core;

// The `#define SATELLITE_* "value"` lines of src/platform/windows/driver_pins.h,
// read as redist-functions.ps1's Get-DriverPins reads them, so check-sdk can
// hold the pins to the SDK the helper ships.
public static partial class DriverPins
{
    public const string SdkVersion = "SATELLITE_HIDMAESTRO_SDK_VERSION";
    public const string VerifiedDriverReleases = "SATELLITE_HIDMAESTRO_VERIFIED_DRIVER_RELEASES";

    public static IReadOnlyDictionary<string, string> Parse(string header)
    {
        var pins = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (Match match in Define().Matches(header))
            pins[match.Groups["name"].Value] = match.Groups["value"].Value;
        return pins;
    }

    // The verified releases: major.minor.patch each, comma-separated, an empty
    // entry naming none. A driver INF's four-part version is not a release.
    public static IReadOnlyList<Version> Releases(string listed)
    {
        var releases = new List<Version>();
        foreach (string entry in listed.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries))
        {
            Match release = Release().Match(entry);
            if (!release.Success) throw new FormatException($"'{entry}' is not a major.minor.patch release");
            releases.Add(new Version(Part(release, "major"), Part(release, "minor"), Part(release, "patch")));
        }
        return releases;
    }

    private static int Part(Match release, string name) =>
        int.Parse(release.Groups[name].Value, NumberStyles.None, CultureInfo.InvariantCulture);

    [GeneratedRegex(@"^[ \t]*#define[ \t]+(?<name>SATELLITE_\w+)[ \t]+""(?<value>[^""]*)""", RegexOptions.Multiline | RegexOptions.CultureInvariant)]
    private static partial Regex Define();

    [GeneratedRegex(@"^(?<major>[0-9]+)\.(?<minor>[0-9]+)\.(?<patch>[0-9]+)$", RegexOptions.CultureInvariant)]
    private static partial Regex Release();
}
