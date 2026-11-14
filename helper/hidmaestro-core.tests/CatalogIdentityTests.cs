// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Text;
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class CatalogIdentityTests
{
    private const string Profile = """
        {
          "id": "dualsense",
          "name": "DualSense (PS5)",
          "stream": { "periodMs": 4, "timestamps": "advance" },
          "inputReports": [ { "name": "simple", "reportId": "0x3F", "send": "change" } ]
        }
        """;

    private static readonly string[] Resources =
    {
        "HIDMaestro.Models.sony-dualsense.json",
        "HIDMaestro.Profiles.sony\\dualsense-edge.json",
        "HIDMaestro.Profiles.sony\\usb-dualsense.json",
        "HIDMaestro.Profiles.sony\\dualsense.json",
        "HIDMaestro.Profiles.nintendo/switch-pro.json",
        "HIDMaestro.Profiles.root-level.json",
    };

    [Fact]
    public void AFileWithCrlfLineEndingsIsTheSameProfile()
    {
        byte[] lf = Utf8(Profile.ReplaceLineEndings("\n"));
        byte[] crlf = Utf8(Profile.ReplaceLineEndings("\r\n"));

        Assert.Empty(CatalogIdentity.DifferingKeys(lf, crlf));
        Assert.Empty(CatalogIdentity.DifferingKeys(crlf, lf));
    }

    [Fact]
    public void AFileWithAByteOrderMarkIsTheSameProfile()
    {
        byte[] plain = Utf8(Profile);
        byte[] marked = Encoding.UTF8.GetPreamble().Concat(plain).ToArray();

        Assert.Empty(CatalogIdentity.DifferingKeys(marked, plain));
        Assert.Empty(CatalogIdentity.DifferingKeys(plain, marked));
    }

    [Fact]
    public void AChangedValueIsADifferenceAtItsTopLevelKey()
    {
        byte[] changed = Utf8(Profile.Replace("\"send\": \"change\"", "\"send\": \"clock\""));

        Assert.Equal(new[] { "inputReports" }, CatalogIdentity.DifferingKeys(Utf8(Profile), changed));
    }

    [Fact]
    public void AMissingKeyIsADifferenceFromEitherSide()
    {
        byte[] withoutStream = Utf8(Profile.Replace("\"stream\": { \"periodMs\": 4, \"timestamps\": \"advance\" },", ""));

        Assert.Equal(new[] { "stream" }, CatalogIdentity.DifferingKeys(withoutStream, Utf8(Profile)));
        Assert.Equal(new[] { "stream" }, CatalogIdentity.DifferingKeys(Utf8(Profile), withoutStream));
    }

    [Fact]
    public void JsonThatIsNotAProfileObjectIsRefused()
    {
        Assert.Throws<ArgumentException>(() => CatalogIdentity.DifferingKeys(Utf8("[1, 2]"), Utf8(Profile)));
    }

    [Fact]
    public void TheEmbeddedProfileIsTheResourceNamedForItsFileUnderTheProfilesPrefix()
    {
        Assert.Equal("HIDMaestro.Profiles.sony\\dualsense.json", CatalogIdentity.ResourceNameFor(Resources, "dualsense"));
        Assert.Equal("HIDMaestro.Profiles.nintendo/switch-pro.json", CatalogIdentity.ResourceNameFor(Resources, "switch-pro"));
        Assert.Equal("HIDMaestro.Profiles.root-level.json", CatalogIdentity.ResourceNameFor(Resources, "root-level"));
    }

    [Fact]
    public void AProfileTheAssemblyDoesNotEmbedIsRefusedAsMissingOrRenamed()
    {
        ArgumentException refused = Assert.Throws<ArgumentException>(() => CatalogIdentity.ResourceNameFor(Resources, "dualshock-4-v2"));

        Assert.Contains("dualshock-4-v2.json", refused.Message);
        Assert.Contains("missing or renamed", refused.Message);
    }

    [Fact]
    public void TwoEmbeddedProfilesWithTheFileNameAreRefused()
    {
        string[] twice = Resources.Append("HIDMaestro.Profiles.sony\\legacy\\dualsense.json").ToArray();

        ArgumentException refused = Assert.Throws<ArgumentException>(() => CatalogIdentity.ResourceNameFor(twice, "dualsense"));

        Assert.Contains("HIDMaestro.Profiles.sony\\dualsense.json", refused.Message);
        Assert.Contains("HIDMaestro.Profiles.sony\\legacy\\dualsense.json", refused.Message);
    }

    private static byte[] Utf8(string text) => Encoding.UTF8.GetBytes(text);
}
