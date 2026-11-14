// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Buffers.Binary;
using System.Runtime.CompilerServices;

namespace Satellite.HmHelper.Core;

// What the device watch found of one pad's devnodes, its worst one first.
public enum DevnodeCondition : uint
{
    Unknown = 0,
    Present = 1,
    Disabled = 2,
    Missing = 3,
    Failed = 4,
    Leaving = 5,
    Starting = 6,
}

public enum WatchedDevnode : uint
{
    Device = 0,
    HidFunction = 1,
    HidCollection = 2,
    Companion = 3,
}

public readonly record struct PadCondition(DevnodeCondition Condition, WatchedDevnode Devnode, uint Problem)
{
    public static readonly PadCondition Present = new(DevnodeCondition.Present, WatchedDevnode.Device, 0);
}

// The 64-byte block satellite.exe maps read-only for one pad
// (src/core/hidmaestro_device_watch.h, whose constants the tests read). The
// watcher writes the condition words under the sequence, odd while it writes;
// the heartbeat has its own writer, which never waits on Plug and Play.
public static class DeviceWatchBlock
{
    public const int BlockSize = 64;
    public const int LayoutOffset = 0;
    public const int SeqOffset = 4;
    public const int HeartbeatOffset = 8;
    public const int ConditionOffset = 12;
    public const int DevnodeOffset = 16;
    public const int ProblemOffset = 20;
    public const int Layout = 1;
    public const int BeatMs = 500;

    public static void Initialize(Span<byte> block)
    {
        Span<byte> whole = Whole(block);
        whole.Clear();
        BinaryPrimitives.WriteUInt32LittleEndian(whole[LayoutOffset..], Layout);
    }

    public static void Write(Span<byte> block, PadCondition found)
    {
        Span<byte> whole = Whole(block);
        uint seq = Volatile.Read(ref Word(whole, SeqOffset));
        Volatile.Write(ref Word(whole, SeqOffset), seq + 1);
        Thread.MemoryBarrier();
        BinaryPrimitives.WriteUInt32LittleEndian(whole[ConditionOffset..], (uint)found.Condition);
        BinaryPrimitives.WriteUInt32LittleEndian(whole[DevnodeOffset..], (uint)found.Devnode);
        BinaryPrimitives.WriteUInt32LittleEndian(whole[ProblemOffset..], found.Problem);
        Thread.MemoryBarrier();
        Volatile.Write(ref Word(whole, SeqOffset), seq + 2);
    }

    public static void Beat(Span<byte> block)
    {
        Span<byte> whole = Whole(block);
        Interlocked.Increment(ref Unsafe.As<uint, int>(ref Word(whole, HeartbeatOffset)));
    }

    // What satellite.exe reads, for the selftest: the heartbeat, and the last
    // finished write, or none while one is under way.
    public readonly record struct Reading(uint Heartbeat, PadCondition? Condition);

    public static Reading Read(ReadOnlySpan<byte> block)
    {
        ReadOnlySpan<byte> whole = Whole(block);
        uint heartbeat = Load(whole, HeartbeatOffset);
        for (int attempt = 0; attempt < ReadAttempts; attempt++)
        {
            uint before = Load(whole, SeqOffset);
            if ((before & 1) != 0) continue;
            Thread.MemoryBarrier();
            var found = new PadCondition((DevnodeCondition)Load(whole, ConditionOffset),
                                         (WatchedDevnode)Load(whole, DevnodeOffset), Load(whole, ProblemOffset));
            Thread.MemoryBarrier();
            if (Load(whole, SeqOffset) == before) return new Reading(heartbeat, found);
        }
        return new Reading(heartbeat, null);
    }

    private const int ReadAttempts = 4;

    private static Span<byte> Whole(Span<byte> block)
    {
        if (block.Length < BlockSize)
            throw new ArgumentException($"a watch block is {BlockSize} bytes, not {block.Length}", nameof(block));
        return block[..BlockSize];
    }

    private static ReadOnlySpan<byte> Whole(ReadOnlySpan<byte> block)
    {
        if (block.Length < BlockSize)
            throw new ArgumentException($"a watch block is {BlockSize} bytes, not {block.Length}", nameof(block));
        return block[..BlockSize];
    }

    private static uint Load(ReadOnlySpan<byte> block, int offset) =>
        Volatile.Read(ref Unsafe.As<byte, uint>(ref Unsafe.AsRef(in block[offset])));

    // Every word is naturally aligned and the block is little-endian, as every
    // Windows target .NET runs on is.
    private static ref uint Word(Span<byte> block, int offset) => ref Unsafe.As<byte, uint>(ref block[offset]);
}

// The hello's protocol: a client that offers none, or 1, is answered exactly
// as before the watch; one that offers 2 or more is served 2.
public static class HelperProtocol
{
    public const int Base = 1;
    public const int DeviceWatch = 2;

    public static int Settle(int? offered) => offered is >= DeviceWatch ? DeviceWatch : Base;

    public static bool Watches(int settled) => settled >= DeviceWatch;
}
