// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Satellite.HmHelper.Core;

public static class SatelliteProfiles
{
    public const string CopyPrefix = "satellite-";
    public const string DirectoryName = "hidmaestro-profiles";

    public static readonly IReadOnlyList<string> StreamFreeCopies = new[]
    {
        "dualsense",
        "dualsense-composite",
        "dualshock-4-v2",
        "dualshock-4-v2-composite",
    };

    private static readonly JsonSerializerOptions WriteOptions = new() { WriteIndented = true };

    public static bool NeedsCopy(string catalogId) => StreamFreeCopies.Contains(catalogId, StringComparer.Ordinal);

    public static string CopyId(string catalogId) => CopyPrefix + catalogId;

    public static string ResolveId(string catalogId) => NeedsCopy(catalogId) ? CopyId(catalogId) : catalogId;

    public static string FileName(string catalogId) => CopyId(catalogId) + ".json";

    public static string IdentityKey(uint serial, string catalogId) => $"satellite:{serial}:{catalogId}";

    public static string Notes(string catalogId) =>
        $"Satellite's copy of HIDMaestro's '{catalogId}' catalog profile without its stream. " +
        "Satellite writes this controller's input section itself and repeats an idle Sony pad's " +
        "frame on its own clock, so the SDK's idle repeat must not write the section too.";

    public static string StripStream(string catalogJson, string catalogId)
    {
        JsonObject profile = JsonNode.Parse(catalogJson) as JsonObject
            ?? throw new ArgumentException("the profile JSON is not an object", nameof(catalogJson));
        string? id = profile["id"] is JsonValue value && value.TryGetValue(out string? text) ? text : null;
        if (id != catalogId)
            throw new ArgumentException($"the profile JSON is '{id ?? "(no id)"}', not '{catalogId}'", nameof(catalogJson));
        profile.Remove("stream");
        profile["id"] = CopyId(catalogId);
        profile["notes"] = Notes(catalogId);
        return profile.ToJsonString(WriteOptions);
    }
}
