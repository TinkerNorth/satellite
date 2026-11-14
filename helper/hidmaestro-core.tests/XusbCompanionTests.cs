// SPDX-License-Identifier: LGPL-3.0-or-later
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class XusbCompanionTests
{
    [Fact]
    public void AProfileThatReachesXInputThroughACompanionHasOne()
    {
        Assert.True(XusbCompanion.Presents("xusbCompanion", usbip: false));
    }

    [Fact]
    public void AProfileThroughXinputhidOrWithNoXInputBlockHasNone()
    {
        Assert.False(XusbCompanion.Presents("xinputhid", usbip: false));
        Assert.False(XusbCompanion.Presents(null, usbip: false));
    }

    [Fact]
    public void TheUsbipPathMakesNoCompanionWhateverTheBlockSays()
    {
        Assert.False(XusbCompanion.Presents("xusbCompanion", usbip: true));
    }

    [Fact]
    public void TheViaIsComparedAsTheSdkComparesItOrdinally()
    {
        Assert.False(XusbCompanion.Presents("XusbCompanion", usbip: false));
        Assert.False(XusbCompanion.Presents("xusbcompanion", usbip: false));
        Assert.False(XusbCompanion.Presents(" xusbCompanion", usbip: false));
    }
}
