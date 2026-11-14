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
            .Concat(Members<WatchedDevnode>("WATCH_DEVNODE_"));

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
                        k.StartsWith("HELPER_PROTOCOL_", StringComparison.Ordinal) ||
                        k.EndsWith("_OFFSET", StringComparison.Ordinal))
            .ToArray();
        Assert.NotEmpty(contract);
        foreach (string name in contract.Append("WATCH_BLOCK_SIZE").Append("WATCH_LAYOUT").Append("WATCH_BEAT_MS"))
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

    [Fact]
    public void AWriteLeavesAnEvenSequenceAndTheFieldsAtTheirOffsets()
    {
        var block = new byte[DeviceWatchBlock.BlockSize];
        DeviceWatchBlock.Initialize(block);
        DeviceWatchBlock.Write(block, new PadCondition(DevnodeCondition.Failed, WatchedDevnode.HidCollection, 43));
        Assert.Equal(2u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.SeqOffset)));
        Assert.Equal(4u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.ConditionOffset)));
        Assert.Equal(2u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.DevnodeOffset)));
        Assert.Equal(43u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.ProblemOffset)));

        DeviceWatchBlock.Write(block, PadCondition.Present);
        Assert.Equal(4u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.SeqOffset)));
        Assert.Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.ConditionOffset)));
        Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.DevnodeOffset)));
        Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.ProblemOffset)));
    }

    [Fact]
    public void ABeatMovesTheHeartbeatAloneAndWrapsAround()
    {
        var block = new byte[DeviceWatchBlock.BlockSize];
        DeviceWatchBlock.Initialize(block);
        DeviceWatchBlock.Write(block, PadCondition.Present);
        byte[] before = (byte[])block.Clone();
        DeviceWatchBlock.Beat(block);
        Assert.Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.HeartbeatOffset)));
        for (int i = 0; i < block.Length; i++)
        {
            bool heartbeat = i >= DeviceWatchBlock.HeartbeatOffset && i < DeviceWatchBlock.HeartbeatOffset + 4;
            if (!heartbeat) Assert.Equal(before[i], block[i]);
        }
        BinaryPrimitives.WriteUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.HeartbeatOffset), uint.MaxValue);
        DeviceWatchBlock.Beat(block);
        Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.HeartbeatOffset)));
    }

    [Fact]
    public void AReadingIsTheHeartbeatAndTheLastFinishedWrite()
    {
        var block = new byte[DeviceWatchBlock.BlockSize];
        DeviceWatchBlock.Initialize(block);
        Assert.Equal(new DeviceWatchBlock.Reading(0, new PadCondition(DevnodeCondition.Unknown, WatchedDevnode.Device, 0)),
                     DeviceWatchBlock.Read(block));
        var disabled = new PadCondition(DevnodeCondition.Disabled, WatchedDevnode.Companion, 22);
        DeviceWatchBlock.Write(block, disabled);
        DeviceWatchBlock.Beat(block);
        Assert.Equal(new DeviceWatchBlock.Reading(1, disabled), DeviceWatchBlock.Read(block));
    }

    [Fact]
    public void AWriteInProgressReadsAsNoCondition()
    {
        var block = new byte[DeviceWatchBlock.BlockSize];
        DeviceWatchBlock.Initialize(block);
        DeviceWatchBlock.Write(block, PadCondition.Present);
        BinaryPrimitives.WriteUInt32LittleEndian(block.AsSpan(DeviceWatchBlock.SeqOffset), 3);
        Assert.Null(DeviceWatchBlock.Read(block).Condition);
    }

    [Fact]
    public void ABlockTooSmallIsRefused()
    {
        Assert.Throws<ArgumentException>(() => DeviceWatchBlock.Initialize(new byte[DeviceWatchBlock.BlockSize - 1]));
        Assert.Throws<ArgumentException>(() => DeviceWatchBlock.Write(new byte[DeviceWatchBlock.BlockSize - 1], PadCondition.Present));
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
