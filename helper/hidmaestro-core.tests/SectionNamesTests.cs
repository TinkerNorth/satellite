// SPDX-License-Identifier: LGPL-3.0-or-later
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class SectionNamesTests
{
    [Fact]
    public void TheNamesFollowTheTransportIndexNotTheSlot()
    {
        Assert.Equal(@"Global\HIDMaestroInput7", SectionNames.Input(7));
        Assert.Equal(@"Global\HIDMaestroInputEvent7", SectionNames.InputEvent(7));
        Assert.Equal(@"Global\HIDMaestroCompanionInputEvent7", SectionNames.CompanionInputEvent(7));
        Assert.Equal(@"Global\HIDMaestroOutput7", SectionNames.Output(7));
        Assert.Equal(@"Global\HIDMaestroOutputEvent7", SectionNames.OutputEvent(7));
    }

    [Fact]
    public void OnlyAnXboxProfileOpensTheCompanionDoorbell()
    {
        Assert.Equal(
            new[]
            {
                @"Global\HIDMaestroInput0", @"Global\HIDMaestroInputEvent0",
                @"Global\HIDMaestroOutput0", @"Global\HIDMaestroOutputEvent0",
            },
            SectionNames.Opened(0, xbox: false));
        Assert.Equal(
            new[]
            {
                @"Global\HIDMaestroInput3", @"Global\HIDMaestroInputEvent3",
                @"Global\HIDMaestroCompanionInputEvent3",
                @"Global\HIDMaestroOutput3", @"Global\HIDMaestroOutputEvent3",
            },
            SectionNames.Opened(3, xbox: true));
    }

    [Fact]
    public void ANameTheSdkDoesNotListIsFound()
    {
        string[] sdk =
        {
            @"Global\HIDMaestroInput3", @"Global\HIDMaestroInputEvent3",
            @"Global\HIDMaestroOutput3", @"Global\HIDMaestroOutputEvent3",
            @"Global\HIDMaestroVersion3", @"\\.\pipe\HIDMaestroOwner3",
        };

        Assert.Null(SectionNames.FirstUnknown(SectionNames.Opened(3, xbox: false), sdk));
        Assert.Equal(@"Global\HIDMaestroCompanionInputEvent3", SectionNames.FirstUnknown(SectionNames.Opened(3, xbox: true), sdk));
        Assert.Equal(@"Global\HIDMaestroInput4", SectionNames.FirstUnknown(SectionNames.Opened(4, xbox: false), sdk));
    }

    [Fact]
    public void ANegativeTransportIndexIsRefused()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => SectionNames.Input(-1));
    }
}
