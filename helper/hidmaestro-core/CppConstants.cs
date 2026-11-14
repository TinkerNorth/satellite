// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Globalization;
using System.Text.RegularExpressions;

namespace Satellite.HmHelper.Core;

// The `inline constexpr <type> NAME = <integer>;` lines of a C++ header, the
// form src/core/hidmaestro_version.h states the version contract in, so
// check-sdk can hold satellite.exe's own values to the SDK's.
public static partial class CppConstants
{
    public static IReadOnlyDictionary<string, long> Parse(string source)
    {
        var constants = new Dictionary<string, long>(StringComparer.Ordinal);
        foreach (Match match in Declaration().Matches(source))
        {
            string name = match.Groups["name"].Value;
            long value = long.Parse(match.Groups["value"].Value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture);
            if (!constants.TryAdd(name, value)) throw new FormatException($"{name} is defined twice");
        }
        return constants;
    }

    [GeneratedRegex(@"^[ \t]*inline[ \t]+constexpr[ \t]+\w+[ \t]+(?<name>[A-Z][A-Z0-9_]*)[ \t]*=[ \t]*(?<value>-?\d+)[ \t]*;",
                    RegexOptions.Multiline | RegexOptions.CultureInvariant)]
    private static partial Regex Declaration();
}
