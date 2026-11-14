// SPDX-License-Identifier: LGPL-3.0-or-later
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class VersionSectionTests
{
    private static readonly byte[] PinnedXbox = Convert.FromHexString(
        "01000000040000000400000002000100" +
        "00000000050000000200010000000000" +
        "03000000040000000400000004000000" +
        "1F468D6B9148D01F0000000000000000");

    [Fact]
    public void ThePinnedSdksSectionOnItsDriverReadsFieldForField()
    {
        VersionSection v = VersionSection.Parse(PinnedXbox);

        Assert.Equal(1, v.LayoutVersion);
        Assert.Equal(4, v.SdkProtocolMin);
        Assert.Equal(4, v.SdkProtocolMax);
        Assert.Equal(new Version(2, 1, 0, 0), v.SdkVersion);
        Assert.Equal(5, v.DriverProtocol);
        Assert.Equal(new Version(2, 1, 0, 0), v.DriverVersion);
        Assert.Equal(3, v.DriverCapabilities);
        Assert.Equal(4, v.DriverProtocolMin);
        Assert.Equal(4, v.CompanionProtocol);
        Assert.Equal(4, v.CompanionProtocolMin);
        Assert.Equal(0x1FD048916B8D461FUL, v.IdentityTag);
        Assert.Equal(0, v.DriverConfigError);
        Assert.Equal(0, v.CompanionConfigError);
    }

    [Fact]
    public void TheErrorWordsAreTheLastTwoWords()
    {
        byte[] failed = (byte[])PinnedXbox.Clone();
        failed[56] = 2;
        failed[60] = 1;

        VersionSection v = VersionSection.Parse(failed);

        Assert.Equal(2, v.DriverConfigError);
        Assert.Equal(1, v.CompanionConfigError);
    }

    [Fact]
    public void AShorterSectionIsRefused()
    {
        Assert.Throws<ArgumentException>(() => VersionSection.Parse(new byte[VersionSection.Size - 1]));
    }
}
