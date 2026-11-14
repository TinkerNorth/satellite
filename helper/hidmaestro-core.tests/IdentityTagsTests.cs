// SPDX-License-Identifier: LGPL-3.0-or-later
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class IdentityTagsTests
{
    [Fact]
    public void ATokensTagIsTheOneHmContractGivesForIt()
    {
        Assert.Equal(0x1FD048916B8D461FUL, IdentityTags.OfToken("HM_0000"));
        Assert.Equal(0x3B86F3634086D831UL, IdentityTags.OfToken("HM_0123456789ABCDEF"));
    }

    [Fact]
    public void AKeyedTokenIsTheBigEndianHeadOfTheKeysSha256InSixteenHexDigits()
    {
        Assert.Equal("HM_642BF518A6392B11", IdentityTags.TokenOf("satellite:1:dualsense"));
        Assert.Equal("HM_E9451CFBBE9EE954", IdentityTags.TokenOf("satellite:3:xbox-360-wired"));
    }

    [Fact]
    public void AKeyIsTrimmedAsTheSdkTrimsIt()
    {
        Assert.Equal(IdentityTags.TokenOf("satellite:1:dualsense"), IdentityTags.TokenOf("  satellite:1:dualsense  "));
    }

    [Fact]
    public void AKeysTagIsItsTokensTag()
    {
        Assert.Equal(0xC356259600512512UL, IdentityTags.OfKey("satellite:1:dualsense"));
        Assert.Equal(0xF2698EBC35709872UL, IdentityTags.OfKey("satellite:3:xbox-360-wired"));
    }

    [Fact]
    public void SatellitesKeysAreNeverTheSdksDefaultForm()
    {
        foreach (string profile in new[] { "xbox-360-wired", "dualsense", "dualsense-composite", "switch-pro" })
        {
            string key = SatelliteProfiles.IdentityKey(3, profile);
            Assert.False(string.IsNullOrWhiteSpace(key));
            Assert.False(key.StartsWith("index:", StringComparison.Ordinal));
        }
    }
}
