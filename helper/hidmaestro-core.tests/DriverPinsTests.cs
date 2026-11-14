// SPDX-License-Identifier: LGPL-3.0-or-later
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class DriverPinsTests
{
    [Fact]
    public void EachSatelliteDefineIsReadWithItsString()
    {
        const string header = """
            #pragma once
            #define SATELLITE_HIDMAESTRO_SDK_VERSION "2.1.0"
              #define SATELLITE_HIDMAESTRO_VERIFIED_DRIVER_RELEASES "2.1.0, 2.2.0"
            // #define SATELLITE_COMMENTED "1"
            #define OTHER_PROJECT_PIN "3"
            """;
        IReadOnlyDictionary<string, string> pins = DriverPins.Parse(header);
        Assert.Equal("2.1.0", pins["SATELLITE_HIDMAESTRO_SDK_VERSION"]);
        Assert.Equal("2.1.0, 2.2.0", pins[DriverPins.VerifiedDriverReleases]);
        Assert.Equal(2, pins.Count);
    }

    [Fact]
    public void AReleaseListReadsEachCommaSeparatedRelease()
    {
        Assert.Equal(new[] { new Version(2, 1, 0) }, DriverPins.Releases("2.1.0"));
        Assert.Equal(new[] { new Version(2, 1, 0), new Version(2, 2, 0), new Version(3, 0, 12) },
                     DriverPins.Releases(" 2.1.0, 2.2.0 ,3.0.12"));
        Assert.Equal(new[] { new Version(2, 1, 0) }, DriverPins.Releases("2.1.0,, "));
    }

    [Fact]
    public void AnEntryThatIsNotAReleaseIsRefused()
    {
        Assert.Throws<FormatException>(() => DriverPins.Releases("2.1"));
        Assert.Throws<FormatException>(() => DriverPins.Releases("2.1.0.1716"));
        Assert.Throws<FormatException>(() => DriverPins.Releases("2.1.0, v2.2.0"));
    }
}
