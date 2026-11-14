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
        Assert.Equal(@"Global\HIDMaestroVersion7", SectionNames.Version(7));
    }

    [Fact]
    public void OnlyAProfileWithACompanionOpensItsDoorbell()
    {
        Assert.Equal(
            new[]
            {
                @"Global\HIDMaestroInput0", @"Global\HIDMaestroInputEvent0",
                @"Global\HIDMaestroOutput0", @"Global\HIDMaestroOutputEvent0",
                @"Global\HIDMaestroVersion0",
            },
            SectionNames.Opened(0, companion: false));
        Assert.Equal(
            new[]
            {
                @"Global\HIDMaestroInput3", @"Global\HIDMaestroInputEvent3",
                @"Global\HIDMaestroCompanionInputEvent3",
                @"Global\HIDMaestroOutput3", @"Global\HIDMaestroOutputEvent3",
                @"Global\HIDMaestroVersion3",
            },
            SectionNames.Opened(3, companion: true));
    }

    [Fact]
    public void TheVersionSectionIsFoundByNameWhereverTheSdkListsIt()
    {
        string[] v210Order =
        {
            @"Global\HIDMaestroInput3", @"Global\HIDMaestroInputEvent3", @"Global\HIDMaestroCompanionInputEvent3",
            @"Global\HIDMaestroInputAck3", @"Global\HIDMaestroInputAckEvent3", @"Global\HIDMaestroRemoveEvent3",
            @"Global\HIDMaestroRemovedEvent3", @"Global\HIDMaestroOutput3", @"Global\HIDMaestroOutputEvent3",
            @"Global\HIDMaestroPidState3", @"Global\HIDMaestroVersion3", @"Global\HIDMaestroSignal3",
            @"\\.\pipe\HIDMaestroOwner3",
        };
        string[] versionFirst = v210Order.OrderBy(name => name.Contains("Version") ? 0 : 1).ToArray();
        string[] withoutVersion = v210Order.Where(name => !name.Contains("Version")).ToArray();

        Assert.Null(SectionNames.FirstUnknown(SectionNames.Opened(3, companion: true), v210Order));
        Assert.Null(SectionNames.FirstUnknown(SectionNames.Opened(3, companion: true), versionFirst));
        Assert.Equal(@"Global\HIDMaestroVersion3", SectionNames.FirstUnknown(SectionNames.Opened(3, companion: false), withoutVersion));
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

        Assert.Null(SectionNames.FirstUnknown(SectionNames.Opened(3, companion: false), sdk));
        Assert.Equal(@"Global\HIDMaestroCompanionInputEvent3", SectionNames.FirstUnknown(SectionNames.Opened(3, companion: true), sdk));
        Assert.Equal(@"Global\HIDMaestroInput4", SectionNames.FirstUnknown(SectionNames.Opened(4, companion: false), sdk));
    }

    [Fact]
    public void ANegativeTransportIndexIsRefused()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => SectionNames.Input(-1));
    }
}
