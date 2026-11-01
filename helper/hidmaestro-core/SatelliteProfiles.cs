// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Satellite.HmHelper.Core;

public enum CopySource
{
    Sdk,
    CatalogFile,
}

public static class SatelliteProfiles
{
    public const string CopyPrefix = "satellite-";
    public const string DirectoryName = "hidmaestro-profiles";
    public const string CatalogDirectoryName = "profiles";

    public static readonly IReadOnlyList<string> StreamFreeCopies = new[]
    {
        "dualsense",
        "dualsense-composite",
        "dualshock-4-v2",
        "dualshock-4-v2-composite",
    };

    public static readonly IReadOnlyList<string> HeartbeatCopies = new[]
    {
        "switch-pro",
    };

    public static readonly IReadOnlyList<string> Copies = StreamFreeCopies.Concat(HeartbeatCopies).ToArray();

    private static readonly JsonSerializerOptions WriteOptions = new() { WriteIndented = true };

    public static bool NeedsCopy(string catalogId) => Copies.Contains(catalogId, StringComparer.Ordinal);

    public static string CopyId(string catalogId) => CopyPrefix + catalogId;

    public static string ResolveId(string catalogId) => NeedsCopy(catalogId) ? CopyId(catalogId) : catalogId;

    public static string FileName(string catalogId) => CopyId(catalogId) + ".json";

    public static string CatalogFileName(string catalogId) => catalogId + ".json";

    public static string IdentityKey(uint serial, string catalogId) => $"satellite:{serial}:{catalogId}";

    public static CopySource SourceOf(string catalogId)
    {
        if (StreamFreeCopies.Contains(catalogId, StringComparer.Ordinal)) return CopySource.Sdk;
        if (HeartbeatCopies.Contains(catalogId, StringComparer.Ordinal)) return CopySource.CatalogFile;
        throw new ArgumentException($"'{catalogId}' is not a profile Satellite copies", nameof(catalogId));
    }

    public static string Notes(string catalogId)
    {
        if (HeartbeatCopies.Contains(catalogId, StringComparer.Ordinal))
            return $"Satellite's copy of HIDMaestro's '{catalogId}' catalog profile with its simple report " +
                   "(0x3F) sent on the device clock instead of on change. Steam and other SDL2-era hosts " +
                   "drop a Bluetooth Switch pad that stays silent for three seconds, and in simple mode " +
                   "an idle pad never changes.";
        return $"Satellite's copy of HIDMaestro's '{catalogId}' catalog profile without its stream. " +
               "Satellite writes this controller's input section itself and repeats an idle Sony pad's " +
               "frame on its own clock, so the SDK's idle repeat must not write the section too.";
    }

    public static string Transform(string catalogJson, string catalogId) =>
        SourceOf(catalogId) == CopySource.CatalogFile
            ? SimpleReportOnClock(catalogJson, catalogId)
            : StripStream(catalogJson, catalogId);

    public static string StripStream(string catalogJson, string catalogId)
    {
        JsonObject profile = Parse(catalogJson, catalogId);
        profile.Remove("stream");
        return Finish(profile, catalogId);
    }

    public static string SimpleReportOnClock(string catalogJson, string catalogId)
    {
        JsonObject profile = Parse(catalogJson, catalogId);
        JsonArray reports = profile["inputReports"] as JsonArray
            ?? throw new ArgumentException($"'{catalogId}' declares no inputReports", nameof(catalogJson));
        JsonObject simple = reports.OfType<JsonObject>().FirstOrDefault(r => r["name"] is JsonValue v && v.TryGetValue(out string? n) && n == "simple")
            ?? throw new ArgumentException($"'{catalogId}' declares no input report named 'simple'", nameof(catalogJson));
        simple["send"] = "clock";
        return Finish(profile, catalogId);
    }

    private static JsonObject Parse(string catalogJson, string catalogId)
    {
        JsonObject profile = JsonNode.Parse(catalogJson) as JsonObject
            ?? throw new ArgumentException("the profile JSON is not an object", nameof(catalogJson));
        string? id = profile["id"] is JsonValue value && value.TryGetValue(out string? text) ? text : null;
        if (id != catalogId)
            throw new ArgumentException($"the profile JSON is '{id ?? "(no id)"}', not '{catalogId}'", nameof(catalogJson));
        return profile;
    }

    private static string Finish(JsonObject profile, string catalogId)
    {
        profile["id"] = CopyId(catalogId);
        profile["notes"] = Notes(catalogId);
        return profile.ToJsonString(WriteOptions);
    }
}
