// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace Satellite.HmHelper;

// The hidmaestro.inf the SDK embeds, which is the driver its InstallDriver
// installs: its DriverVer, as `sdk-version` prints it and check-sdk holds to
// the releases driver_pins.h has verified.
internal static class SdkInf
{
    private static readonly Regex DriverVerLine =
        new(@"^\s*DriverVer\s*=\s*[0-9/]+\s*,\s*([0-9]+(?:\.[0-9]+)*)", RegexOptions.Compiled);

    public static string EmbeddedDriverVersion(Assembly sdk)
    {
        string[] infs = sdk.GetManifestResourceNames()
            .Where(n => n.EndsWith("hidmaestro.inf", StringComparison.OrdinalIgnoreCase))
            .ToArray();
        string arch = $".{RuntimeInformation.ProcessArchitecture}.";
        string name = infs.FirstOrDefault(n => n.Contains(arch, StringComparison.OrdinalIgnoreCase))
            ?? infs.FirstOrDefault()
            ?? throw new InvalidOperationException("the SDK embeds no hidmaestro.inf resource");
        using Stream inf = sdk.GetManifestResourceStream(name)
            ?? throw new InvalidOperationException($"{name} could not be opened");
        using var reader = new StreamReader(inf, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
        while (reader.ReadLine() is string line)
        {
            Match match = DriverVerLine.Match(line);
            if (match.Success) return match.Groups[1].Value;
        }
        throw new InvalidOperationException($"{name} has no DriverVer line");
    }
}
