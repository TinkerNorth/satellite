// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Buffers.Binary;
using System.Reflection;
using System.Text;
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class DeviceWatchBlockTests
{
    // src/core/hidmaestro_device_watch.h, which the build copies beside the tests.
    private static IReadOnlyDictionary<string, long> Header() =>
        CppConstants.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "cpp", "hidmaestro_device_watch.h")));

    private static string UpperSnake(string name)
    {
        var text = new StringBuilder();
        for (int i = 0; i < name.Length; i++)
        {
            bool word = i > 0 && char.IsUpper(name[i]) && !char.IsUpper(name[i - 1]);
            if (word) text.Append('_');
            text.Append(char.ToUpperInvariant(name[i]));
        }
        return text.ToString();
    }

    private static IEnumerable<(string Header, long Value)> Constants(Type type, string prefix) =>
        type.GetFields(BindingFlags.Public | BindingFlags.Static)
            .Where(f => f.IsLiteral)
            .Select(f => (prefix + UpperSnake(f.Name), Convert.ToInt64(f.GetRawConstantValue())));

    private static IEnumerable<(string Header, long Value)> Members<T>(string prefix) where T : struct, Enum =>
        Enum.GetValues<T>().Select(v => (prefix + UpperSnake(v.ToString()), Convert.ToInt64(v)));

    private static IEnumerable<(string Header, long Value)> Stated() =>
        Constants(typeof(DeviceWatchBlock), "WATCH_")
            .Concat(Constants(typeof(HelperProtocol), "HELPER_PROTOCOL_"))
            .Concat(Members<DevnodeCondition>("WATCH_CONDITION_"))
            .Concat(Members<WatchedDevnode>("WATCH_DEVNODE_"))
            .Concat(Members<Presents>("WATCH_PRESENTS_"));

    [Fact]
    public void EveryValueTheWriterUsesIsTheOneSatelliteReads()
    {
        IReadOnlyDictionary<string, long> header = Header();
        foreach ((string name, long value) in Stated())
        {
            Assert.True(header.ContainsKey(name), $"hidmaestro_device_watch.h states no {name}");
            Assert.True(header[name] == value, $"{name} is {header[name]} in the header and {value} here");
        }
    }

    [Fact]
    public void EveryCodeAndOffsetTheReaderKnowsHasAWriter()
    {
        var stated = Stated().Select(c => c.Header).ToHashSet(StringComparer.Ordinal);
        string[] contract = Header().Keys
            .Where(k => k.StartsWith("WATCH_CONDITION_", StringComparison.Ordinal) ||
                        k.StartsWith("WATCH_DEVNODE_", StringComparison.Ordinal) ||
                        k.StartsWith("WATCH_PRESENTS_", StringComparison.Ordinal) ||
                        k.StartsWith("HELPER_PROTOCOL_", StringComparison.Ordinal) ||
                        k.EndsWith("_OFFSET", StringComparison.Ordinal))
            .ToArray();
        Assert.NotEmpty(contract);
        string[] alsoNeeded = { "WATCH_BLOCK_SIZE", "WATCH_LAYOUT", "WATCH_BEAT_MS", "WATCH_PRESENTATION_SIZE" };
        foreach (string name in contract.Concat(alsoNeeded))
            Assert.True(stated.Contains(name), $"the helper has no counterpart of {name}");
    }

    [Fact]
    public void AFreshBlockIsLayoutOneWithNothingLookedAt()
    {
        var block = Enumerable.Repeat((byte)0xAA, DeviceWatchBlock.BlockSize).ToArray();
        DeviceWatchBlock.Initialize(block);
        Assert.Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.LayoutOffset)));
        Assert.All(block.Skip(4), b => Assert.Equal(0, b));
    }

    private static readonly PresentationCondition PresentHid = new(DevnodeCondition.Present, WatchedDevnode.HidCollection, 0);

    private static uint At(byte[] block, int offset) => BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(offset));

    [Fact]
    public void AWriteLeavesAnEvenSequenceAndEachPresentationAtItsOffsets()
    {
        var block = new byte[DeviceWatchBlock.BlockSize];
        DeviceWatchBlock.Initialize(block);
        var report = new PadReport(Presents.Hid | Presents.Xinput | Presents.Audio,
                                   new PresentationCondition(DevnodeCondition.Failed, WatchedDevnode.HidCollection, 43),
                                   new PresentationCondition(DevnodeCondition.Disabled, WatchedDevnode.Companion, 22),
                                   new PresentationCondition(DevnodeCondition.Missing, WatchedDevnode.AudioFunction, 0));
        DeviceWatchBlock.Write(block, report);
        Assert.Equal(2u, At(block, DeviceWatchBlock.SeqOffset));
        Assert.Equal(7u, At(block, DeviceWatchBlock.PresentsOffset));
        Assert.Equal(4u, At(block, DeviceWatchBlock.HidOffset + DeviceWatchBlock.FieldConditionOffset));
        Assert.Equal(2u, At(block, DeviceWatchBlock.HidOffset + DeviceWatchBlock.FieldDevnodeOffset));
        Assert.Equal(43u, At(block, DeviceWatchBlock.HidOffset + DeviceWatchBlock.FieldProblemOffset));
        Assert.Equal(2u, At(block, DeviceWatchBlock.XinputOffset + DeviceWatchBlock.FieldConditionOffset));
        Assert.Equal(3u, At(block, DeviceWatchBlock.XinputOffset + DeviceWatchBlock.FieldDevnodeOffset));
        Assert.Equal(22u, At(block, DeviceWatchBlock.XinputOffset + DeviceWatchBlock.FieldProblemOffset));
        Assert.Equal(3u, At(block, DeviceWatchBlock.AudioOffset + DeviceWatchBlock.FieldConditionOffset));
        Assert.Equal(4u, At(block, DeviceWatchBlock.AudioOffset + DeviceWatchBlock.FieldDevnodeOffset));

        DeviceWatchBlock.Write(block, new PadReport(Presents.Hid, PresentHid, default, default));
        Assert.Equal(4u, At(block, DeviceWatchBlock.SeqOffset));
        Assert.Equal(1u, At(block, DeviceWatchBlock.PresentsOffset));
        Assert.Equal(1u, At(block, DeviceWatchBlock.HidOffset + DeviceWatchBlock.FieldConditionOffset));
        Assert.Equal(0u, At(block, DeviceWatchBlock.XinputOffset + DeviceWatchBlock.FieldConditionOffset));
    }

    [Fact]
    public void EveryLookMovesTheSequenceThoughNothingChanged()
    {
        var block = new byte[DeviceWatchBlock.BlockSize];
        DeviceWatchBlock.Initialize(block);
        var report = new PadReport(Presents.Hid, PresentHid, default, default);
        DeviceWatchBlock.Write(block, report);
        DeviceWatchBlock.Write(block, report);
        Assert.Equal(4u, At(block, DeviceWatchBlock.SeqOffset));
    }

    [Fact]
    public void ABeatMovesTheHeartbeatAloneAndWrapsAround()
    {
        var block = new byte[DeviceWatchBlock.BlockSize];
        DeviceWatchBlock.Initialize(block);
        DeviceWatchBlock.Write(block, new PadReport(Presents.Hid, PresentHid, default, default));
        byte[] before = (byte[])block.Clone();
        DeviceWatchBlock.Beat(block);
        Assert.Equal(1u, At(block, DeviceWatchBlock.HeartbeatOffset));
        for (int i = 0; i < block.Length; i++)
        {
            bool heartbeat = i >= DeviceWatchBlock.HeartbeatOffset && i < DeviceWatchBlock.HeartbeatOffset + 4;
            if (!heartbeat) Assert.Equal(before[i], block[i]);
        }
        BinaryPrimitives.WriteUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.HeartbeatOffset), uint.MaxValue);
        DeviceWatchBlock.Beat(block);
        Assert.Equal(0u, At(block, DeviceWatchBlock.HeartbeatOffset));
    }

    [Fact]
    public void AReadingIsTheHeartbeatTheSequenceAndTheLastFinishedWrite()
    {
        var block = new byte[DeviceWatchBlock.BlockSize];
        DeviceWatchBlock.Initialize(block);
        Assert.Equal(new DeviceWatchBlock.Reading(0, 0, new PadReport(0, default, default, default)),
                     DeviceWatchBlock.Read(block));
        var report = new PadReport(Presents.Hid | Presents.Xinput, PresentHid,
                                   new PresentationCondition(DevnodeCondition.Disabled, WatchedDevnode.Companion, 22),
                                   default);
        DeviceWatchBlock.Write(block, report);
        DeviceWatchBlock.Beat(block);
        Assert.Equal(new DeviceWatchBlock.Reading(1, 2, report), DeviceWatchBlock.Read(block));
    }

    [Fact]
    public void AWriteInProgressReadsAsNoReport()
    {
        var block = new byte[DeviceWatchBlock.BlockSize];
        DeviceWatchBlock.Initialize(block);
        DeviceWatchBlock.Write(block, new PadReport(Presents.Hid, PresentHid, default, default));
        BinaryPrimitives.WriteUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.SeqOffset), 3);
        Assert.Null(DeviceWatchBlock.Read(block).Report);
    }

    [Fact]
    public void AReportIsWholeOnlyWhenEveryPresentationItHasIsPresent()
    {
        var presentCompanion = new PresentationCondition(DevnodeCondition.Present, WatchedDevnode.Companion, 0);
        var disabledCompanion = new PresentationCondition(DevnodeCondition.Disabled, WatchedDevnode.Companion, 22);
        Assert.True(new PadReport(Presents.Hid, PresentHid, default, default).Whole);
        Assert.True(new PadReport(Presents.Hid | Presents.Xinput, PresentHid, presentCompanion, default).Whole);
        Assert.False(new PadReport(Presents.Hid | Presents.Xinput, PresentHid, disabledCompanion, default).Whole);
        Assert.False(new PadReport(Presents.Hid | Presents.Audio, PresentHid, default, default).Whole);
    }

    [Fact]
    public void ABlockTooSmallIsRefused()
    {
        Assert.Throws<ArgumentException>(() => DeviceWatchBlock.Initialize(new byte[DeviceWatchBlock.BlockSize - 1]));
        Assert.Throws<ArgumentException>(() => DeviceWatchBlock.Write(new byte[DeviceWatchBlock.BlockSize - 1],
                                                                      new PadReport(Presents.Hid, PresentHid, default, default)));
        Assert.Throws<ArgumentException>(() => DeviceWatchBlock.Beat(new byte[DeviceWatchBlock.BlockSize - 1]));
        Assert.Throws<ArgumentException>(() => DeviceWatchBlock.Read(new byte[DeviceWatchBlock.BlockSize - 1]));
    }

    [Fact]
    public void AClientThatOffersNoProtocolOrTheBaseOneGetsTheBaseOne()
    {
        Assert.Equal(HelperProtocol.Base, HelperProtocol.Settle(null));
        Assert.Equal(HelperProtocol.Base, HelperProtocol.Settle(1));
        Assert.Equal(HelperProtocol.Base, HelperProtocol.Settle(0));
        Assert.False(HelperProtocol.Watches(HelperProtocol.Settle(1)));
    }

    [Fact]
    public void AClientThatOffersTheWatchOrANewerProtocolGetsTheWatch()
    {
        Assert.Equal(HelperProtocol.DeviceWatch, HelperProtocol.Settle(2));
        Assert.Equal(HelperProtocol.DeviceWatch, HelperProtocol.Settle(9));
        Assert.True(HelperProtocol.Watches(HelperProtocol.Settle(2)));
    }
}
