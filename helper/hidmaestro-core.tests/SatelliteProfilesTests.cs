// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Text.Json.Nodes;
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class SatelliteProfilesTests
{
    private const string Catalog = """
        {
          "id": "dualsense",
          "name": "DualSense (PS5)",
          "vid": "0x054C",
          "deviceModelFrom": "sony-dualsense",
          "extendedReport": { "reportId": "0x01", "size": 64, "fields": [ { "byte": 1, "type": "uint8-axis" } ] },
          "notes": "upstream notes",
          "stream": { "periodMs": 4, "timestamps": "advance" },
          "layout": { "kind": "gamepad" }
        }
        """;

    [Fact]
    public void TheFourSonyProfilesSatelliteWritesNativelyGetCopies()
    {
        Assert.Equal(
            new[] { "dualsense", "dualsense-composite", "dualshock-4-v2", "dualshock-4-v2-composite" },
            SatelliteProfiles.StreamFreeCopies);
        foreach (string id in SatelliteProfiles.StreamFreeCopies) Assert.True(SatelliteProfiles.NeedsCopy(id));
        Assert.False(SatelliteProfiles.NeedsCopy("switch-pro"));
        Assert.False(SatelliteProfiles.NeedsCopy("xbox-360-wired"));
    }

    [Fact]
    public void ACopyIsNamedAfterItsCatalogProfile()
    {
        Assert.Equal("satellite-dualsense", SatelliteProfiles.CopyId("dualsense"));
        Assert.Equal("satellite-dualsense.json", SatelliteProfiles.FileName("dualsense"));
        Assert.Equal("satellite-dualshock-4-v2-composite", SatelliteProfiles.ResolveId("dualshock-4-v2-composite"));
        Assert.Equal("switch-pro", SatelliteProfiles.ResolveId("switch-pro"));
        Assert.Equal("hidmaestro-profiles", SatelliteProfiles.DirectoryName);
    }

    [Fact]
    public void AnIdentityKeyNamesTheSlotAndTheCatalogProfile()
    {
        Assert.Equal("satellite:3:dualsense", SatelliteProfiles.IdentityKey(3, "dualsense"));
        Assert.Equal("satellite:12:switch-pro", SatelliteProfiles.IdentityKey(12, "switch-pro"));
    }

    [Fact]
    public void StripStreamRemovesTheStreamAndRenamesTheProfile()
    {
        JsonObject copy = JsonNode.Parse(SatelliteProfiles.StripStream(Catalog, "dualsense"))!.AsObject();

        Assert.Null(copy["stream"]);
        Assert.Equal("satellite-dualsense", copy["id"]!.GetValue<string>());
        Assert.Equal(SatelliteProfiles.Notes("dualsense"), copy["notes"]!.GetValue<string>());
        Assert.Contains("dualsense", SatelliteProfiles.Notes("dualsense"));
    }

    [Fact]
    public void StripStreamKeepsEveryOtherKeyAndValue()
    {
        JsonObject original = JsonNode.Parse(Catalog)!.AsObject();
        JsonObject copy = JsonNode.Parse(SatelliteProfiles.StripStream(Catalog, "dualsense"))!.AsObject();

        foreach (KeyValuePair<string, JsonNode?> pair in original)
        {
            if (pair.Key is "stream") continue;
            if (pair.Key is "id" or "notes") continue;
            Assert.True(JsonNode.DeepEquals(pair.Value, copy[pair.Key]), pair.Key);
        }
        Assert.Equal(original.Count - 1, copy.Count);
    }

    [Fact]
    public void StripStreamKeepsTheKeyOrder()
    {
        JsonObject copy = JsonNode.Parse(SatelliteProfiles.StripStream(Catalog, "dualsense"))!.AsObject();

        Assert.Equal(
            new[] { "id", "name", "vid", "deviceModelFrom", "extendedReport", "notes", "layout" },
            copy.Select(pair => pair.Key).ToArray());
    }

    [Fact]
    public void StripStreamOnAProfileWithoutAStreamStillMakesTheCopy()
    {
        JsonObject original = JsonNode.Parse(Catalog)!.AsObject();
        original.Remove("stream");

        JsonObject copy = JsonNode.Parse(SatelliteProfiles.StripStream(original.ToJsonString(), "dualsense"))!.AsObject();

        Assert.Equal("satellite-dualsense", copy["id"]!.GetValue<string>());
        Assert.Null(copy["stream"]);
    }

    [Fact]
    public void StripStreamRefusesAnotherProfilesJson()
    {
        ArgumentException refused = Assert.Throws<ArgumentException>(() => SatelliteProfiles.StripStream(Catalog, "dualshock-4-v2"));
        Assert.Contains("dualsense", refused.Message);
        Assert.Contains("dualshock-4-v2", refused.Message);
    }

    [Fact]
    public void StripStreamRefusesJsonThatIsNotAProfile()
    {
        Assert.Throws<ArgumentException>(() => SatelliteProfiles.StripStream("[1, 2]", "dualsense"));
        Assert.Throws<ArgumentException>(() => SatelliteProfiles.StripStream("{ \"name\": \"no id\" }", "dualsense"));
    }

    [Fact]
    public void TheCopyIsIndentedJsonAProgramCanReadBack()
    {
        string text = SatelliteProfiles.StripStream(Catalog, "dualsense");

        Assert.Contains("\n", text);
        Assert.NotNull(JsonNode.Parse(text));
    }
}
