// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Text;
using System.Text.Json.Nodes;

namespace Satellite.HmHelper.Core;

public static class CatalogIdentity
{
    public const string ResourcePrefix = "HIDMaestro.Profiles.";

    private static readonly char[] PathSeparators = { '\\', '/' };

    public static string ResourceNameFor(IEnumerable<string> resourceNames, string catalogId)
    {
        string fileName = SatelliteProfiles.CatalogFileName(catalogId);
        string[] matches = resourceNames.Where(name => NamesProfileFile(name, fileName)).ToArray();
        if (matches.Length == 0)
            throw new ArgumentException($"HIDMaestro.Core.dll embeds no {ResourcePrefix}<dir>\\{fileName}: the catalog profile is missing or renamed", nameof(resourceNames));
        if (matches.Length > 1)
            throw new ArgumentException($"HIDMaestro.Core.dll embeds {fileName} {matches.Length} times: {string.Join(", ", matches)}", nameof(resourceNames));
        return matches[0];
    }

    public static IReadOnlyList<string> DifferingKeys(ReadOnlySpan<byte> catalogFile, ReadOnlySpan<byte> embedded)
    {
        JsonObject file = ParseProfile(catalogFile);
        JsonObject resource = ParseProfile(embedded);
        return file.Select(pair => pair.Key)
            .Union(resource.Select(pair => pair.Key))
            .Where(key => file.ContainsKey(key) != resource.ContainsKey(key) || !JsonNode.DeepEquals(file[key], resource[key]))
            .ToArray();
    }

    private static bool NamesProfileFile(string resourceName, string fileName) =>
        resourceName.StartsWith(ResourcePrefix, StringComparison.Ordinal)
        && LastSegment(resourceName[ResourcePrefix.Length..]) == fileName;

    private static string LastSegment(string path) => path[(path.LastIndexOfAny(PathSeparators) + 1)..];

    private static JsonObject ParseProfile(ReadOnlySpan<byte> json)
    {
        ReadOnlySpan<byte> bom = Encoding.UTF8.Preamble;
        ReadOnlySpan<byte> body = json.StartsWith(bom) ? json[bom.Length..] : json;
        return JsonNode.Parse(body) as JsonObject
            ?? throw new ArgumentException("the profile JSON is not an object", nameof(json));
    }
}
