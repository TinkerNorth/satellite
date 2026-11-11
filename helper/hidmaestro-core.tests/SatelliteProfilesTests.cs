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

    private const string SwitchPro = """
        {
          "id": "switch-pro",
          "name": "Nintendo Switch Pro Controller",
          "vid": "0x057E",
          "notes": "upstream notes",
          "stateBlock": { "size": 48, "fields": [] },
          "inputReports": [
            { "name": "full", "reportId": "0x30", "size": 49, "while": "protocolSeen && inputMode in 30,31", "send": "clock", "program": [] },
            { "name": "simple", "reportId": "0x3F", "size": 12, "while": "!protocolSeen || inputMode==3F", "send": "change", "program": [] }
          ],
          "protocol": { "input": { "clock": { "periodMs": 15, "origin": "attach" } } }
        }
        """;

    [Fact]
    public void TheCopiesAreTheSixSonyProfilesAndTheSwitchPro()
    {
        Assert.Equal(
            new[]
            {
                "dualsense", "dualsense-composite", "dualsense-edge", "dualsense-edge-composite",
                "dualshock-4-v2", "dualshock-4-v2-composite",
            },
            SatelliteProfiles.StreamFreeCopies);
        Assert.Equal(new[] { "switch-pro" }, SatelliteProfiles.HeartbeatCopies);
        Assert.Equal(
            new[]
            {
                "dualsense", "dualsense-composite", "dualsense-edge", "dualsense-edge-composite",
                "dualshock-4-v2", "dualshock-4-v2-composite", "switch-pro",
            },
            SatelliteProfiles.Copies);
        foreach (string id in SatelliteProfiles.Copies) Assert.True(SatelliteProfiles.NeedsCopy(id));
        Assert.False(SatelliteProfiles.NeedsCopy("xbox-360-wired"));
    }

    [Fact]
    public void TheSonyCopiesComeFromTheSdkAndTheSwitchProFromTheCatalogFile()
    {
        foreach (string id in SatelliteProfiles.StreamFreeCopies) Assert.Equal(CopySource.Sdk, SatelliteProfiles.SourceOf(id));
        Assert.Equal(CopySource.CatalogFile, SatelliteProfiles.SourceOf("switch-pro"));
        Assert.Throws<ArgumentException>(() => SatelliteProfiles.SourceOf("xbox-360-wired"));
        Assert.Equal("switch-pro.json", SatelliteProfiles.CatalogFileName("switch-pro"));
        Assert.Equal("profiles", SatelliteProfiles.CatalogDirectoryName);
    }

    [Fact]
    public void ACopyIsNamedAfterItsCatalogProfile()
    {
        Assert.Equal("satellite-dualsense", SatelliteProfiles.CopyId("dualsense"));
        Assert.Equal("satellite-dualsense.json", SatelliteProfiles.FileName("dualsense"));
        Assert.Equal("satellite-dualshock-4-v2-composite", SatelliteProfiles.ResolveId("dualshock-4-v2-composite"));
        Assert.Equal("satellite-switch-pro", SatelliteProfiles.ResolveId("switch-pro"));
        Assert.Equal("satellite-dualsense-edge-composite", SatelliteProfiles.ResolveId("dualsense-edge-composite"));
        Assert.Equal("xbox-360-wired", SatelliteProfiles.ResolveId("xbox-360-wired"));
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
            if (pair.Key is "stream" or "id" or "notes") continue;
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
    public void SimpleReportOnClockChangesOnlyTheSimpleReportsSendRule()
    {
        JsonObject original = JsonNode.Parse(SwitchPro)!.AsObject();
        JsonObject copy = JsonNode.Parse(SatelliteProfiles.SimpleReportOnClock(SwitchPro, "switch-pro"))!.AsObject();

        JsonArray reports = copy["inputReports"]!.AsArray();
        JsonObject full = reports[0]!.AsObject();
        JsonObject simple = reports[1]!.AsObject();
        Assert.Equal("clock", simple["send"]!.GetValue<string>());
        Assert.Equal("0x3F", simple["reportId"]!.GetValue<string>());
        Assert.Equal("!protocolSeen || inputMode==3F", simple["while"]!.GetValue<string>());
        Assert.True(JsonNode.DeepEquals(original["inputReports"]![0], full));
        Assert.Equal("satellite-switch-pro", copy["id"]!.GetValue<string>());
        Assert.Equal(SatelliteProfiles.Notes("switch-pro"), copy["notes"]!.GetValue<string>());
        Assert.Contains("three seconds", SatelliteProfiles.Notes("switch-pro"));
        foreach (KeyValuePair<string, JsonNode?> pair in original)
        {
            if (pair.Key is "inputReports" or "id" or "notes") continue;
            Assert.True(JsonNode.DeepEquals(pair.Value, copy[pair.Key]), pair.Key);
        }
        Assert.Equal(original.Count, copy.Count);
    }

    [Fact]
    public void SimpleReportOnClockRefusesAProfileWithoutASimpleReport()
    {
        JsonObject original = JsonNode.Parse(SwitchPro)!.AsObject();
        original["inputReports"]!.AsArray().RemoveAt(1);
        Assert.Throws<ArgumentException>(() => SatelliteProfiles.SimpleReportOnClock(original.ToJsonString(), "switch-pro"));

        original.Remove("inputReports");
        Assert.Throws<ArgumentException>(() => SatelliteProfiles.SimpleReportOnClock(original.ToJsonString(), "switch-pro"));
    }

    [Fact]
    public void TransformPicksTheTransformForTheProfile()
    {
        JsonObject sony = JsonNode.Parse(SatelliteProfiles.Transform(Catalog, "dualsense"))!.AsObject();
        JsonObject nintendo = JsonNode.Parse(SatelliteProfiles.Transform(SwitchPro, "switch-pro"))!.AsObject();

        Assert.Null(sony["stream"]);
        Assert.Equal("clock", nintendo["inputReports"]![1]!["send"]!.GetValue<string>());
        Assert.Throws<ArgumentException>(() => SatelliteProfiles.Transform(Catalog, "xbox-360-wired"));
    }

    [Fact]
    public void ATransformRefusesAnotherProfilesJson()
    {
        ArgumentException refused = Assert.Throws<ArgumentException>(() => SatelliteProfiles.StripStream(Catalog, "dualshock-4-v2"));
        Assert.Contains("dualsense", refused.Message);
        Assert.Contains("dualshock-4-v2", refused.Message);
        Assert.Throws<ArgumentException>(() => SatelliteProfiles.SimpleReportOnClock(Catalog, "switch-pro"));
    }

    [Fact]
    public void ATransformRefusesJsonThatIsNotAProfile()
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
