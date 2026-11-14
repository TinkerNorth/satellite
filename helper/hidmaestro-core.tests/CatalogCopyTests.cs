// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class PinnedCatalog
{
    private static readonly Regex CatalogPin =
        new(@"^(?<hash>[0-9a-f]{64}) \*HIDMaestro-v\d+\.\d+\.\d+-(?<id>[a-z0-9-]+)\.json$");

    public PinnedCatalog()
    {
        string redist = Path.Combine(AppContext.BaseDirectory, "redist");
        Dir = Path.Combine(redist, "hidmaestro", SatelliteProfiles.CatalogDirectoryName);
        Pins = File.ReadLines(Path.Combine(redist, "SHA256SUMS"))
            .Select(line => CatalogPin.Match(line))
            .Where(match => match.Success)
            .ToLookup(match => match.Groups["id"].Value, match => match.Groups["hash"].Value);
    }

    public string Dir { get; }
    public ILookup<string, string> Pins { get; }

    public string PathOf(string catalogId)
    {
        string path = Path.Combine(Dir, SatelliteProfiles.CatalogFileName(catalogId));
        return File.Exists(path)
            ? path
            : throw new FileNotFoundException($"{path} is missing: scripts/fetch-redist.ps1 stages the pinned catalog files", path);
    }

    public JsonObject Read(string catalogId) => JsonNode.Parse(File.ReadAllText(PathOf(catalogId)))!.AsObject();
}

public sealed class ExportedCopies : IDisposable
{
    public ExportedCopies()
    {
        Catalog = new PinnedCatalog();
        OutDir = Directory.CreateTempSubdirectory("satellite-hm-copies-").FullName;
        Paths = SatelliteProfiles.Export(OutDir, Catalog.Dir);
    }

    public PinnedCatalog Catalog { get; }
    public string OutDir { get; }
    public IReadOnlyList<string> Paths { get; }

    public JsonObject Read(string catalogId) =>
        JsonNode.Parse(File.ReadAllText(Path.Combine(OutDir, SatelliteProfiles.FileName(catalogId))))!.AsObject();

    public void Dispose() => Directory.Delete(OutDir, recursive: true);
}

public sealed class CatalogCopyTests : IClassFixture<ExportedCopies>
{
    private readonly ExportedCopies _copies;

    public CatalogCopyTests(ExportedCopies copies) => _copies = copies;

    public static TheoryData<string> AllCopies => new(SatelliteProfiles.Copies);

    public static TheoryData<string> SonyCopies => new(SatelliteProfiles.StreamFreeCopies);

    [Theory]
    [MemberData(nameof(AllCopies))]
    public void ACopyIsCutFromTheCatalogFileSha256SumsPins(string catalogId)
    {
        string pin = Assert.Single(_copies.Catalog.Pins[catalogId]);

        Assert.Equal(pin, Sha256Of(_copies.Catalog.PathOf(catalogId)));
    }

    [Fact]
    public void TheExportWritesOneFilePerCopy()
    {
        string[] expected = SatelliteProfiles.Copies.Select(SatelliteProfiles.FileName).ToArray();

        Assert.Equal(expected, _copies.Paths.Select(Path.GetFileName).ToArray());
        Assert.Equal(expected.Order(StringComparer.Ordinal), Directory.GetFiles(_copies.OutDir).Select(Path.GetFileName).Order(StringComparer.Ordinal));
    }

    [Theory]
    [MemberData(nameof(SonyCopies))]
    public void ASonyCopyIsItsCatalogFileWithoutTheStream(string catalogId)
    {
        JsonObject expected = Renamed(_copies.Catalog.Read(catalogId), catalogId);

        Assert.True(expected.Remove("stream"), $"the catalog '{catalogId}' carries no stream for its copy to leave out");
        AssertSameProfile(expected, _copies.Read(catalogId));
    }

    [Fact]
    public void TheSwitchProCopyIsItsCatalogFileWithTheSimpleReportOnTheClock()
    {
        JsonObject expected = Renamed(_copies.Catalog.Read("switch-pro"), "switch-pro");
        JsonObject simple = SimpleReport(expected);

        Assert.Equal("change", simple["send"]!.GetValue<string>());
        simple["send"] = "clock";
        AssertSameProfile(expected, _copies.Read("switch-pro"));
    }

    private static JsonObject Renamed(JsonObject catalog, string catalogId)
    {
        catalog["id"] = "satellite-" + catalogId;
        catalog["notes"] = SatelliteProfiles.Notes(catalogId);
        return catalog;
    }

    private static JsonObject SimpleReport(JsonObject profile) =>
        Assert.Single(profile["inputReports"]!.AsArray(), report => report!["name"]!.GetValue<string>() == "simple")!.AsObject();

    private static void AssertSameProfile(JsonObject expected, JsonObject copy) =>
        Assert.True(JsonNode.DeepEquals(expected, copy), "the copy differs from its catalog file at: " + string.Join(", ", DifferingKeys(expected, copy)));

    private static IEnumerable<string> DifferingKeys(JsonObject expected, JsonObject copy) =>
        expected.Select(pair => pair.Key)
            .Union(copy.Select(pair => pair.Key))
            .Where(key => expected.ContainsKey(key) != copy.ContainsKey(key) || !JsonNode.DeepEquals(expected[key], copy[key]));

    private static string Sha256Of(string path) => Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)));
}
