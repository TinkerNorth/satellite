// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Buffers.Binary;
using System.Runtime.CompilerServices;

namespace Satellite.HmHelper.Core;

// What the device watch found of one of a pad's presentations: its devnodes
// from the top, the first that is not present naming the condition.
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
    AudioFunction = 4,
}

// The ways a pad presents itself to Windows, from the SDK's create plan: every
// pad a HID game controller, a profile with an XUSB companion an XInput device
// beside it, a composite a USB-audio function.
[Flags]
public enum Presents : uint
{
    Hid = 1,
    Xinput = 2,
    Audio = 4,
}

public readonly record struct PresentationCondition(DevnodeCondition Condition, WatchedDevnode Devnode, uint Problem);

// What one look found of each of a pad's presentations; one the pad lacks is
// left unknown.
public readonly record struct PadReport(Presents Presents, PresentationCondition Hid, PresentationCondition Xinput,
                                        PresentationCondition Audio)
{
    public bool Present(Presents part) => Presents.HasFlag(part) && Of(part).Condition == DevnodeCondition.Present;

    public PresentationCondition Of(Presents part) => part switch
    {
        Presents.Xinput => Xinput,
        Presents.Audio => Audio,
        _ => Hid,
    };

    // Every presentation the pad has is present.
    public bool Whole => Present(Presents.Hid) && (!Presents.HasFlag(Presents.Xinput) || Present(Presents.Xinput)) &&
                         (!Presents.HasFlag(Presents.Audio) || Present(Presents.Audio));
}

// The 64-byte block satellite.exe maps read-only for one pad
// (src/core/hidmaestro_device_watch.h, whose constants the tests read). The
// watcher writes everything but the heartbeat under the sequence, odd while it
// writes, once per look; the heartbeat has its own writer, which never waits
// on Plug and Play.
public static class DeviceWatchBlock
{
    public const int BlockSize = 64;
    public const int LayoutOffset = 0;
    public const int SeqOffset = 4;
    public const int HeartbeatOffset = 8;
    public const int PresentsOffset = 12;
    public const int HidOffset = 16;
    public const int XinputOffset = 28;
    public const int AudioOffset = 40;
    public const int PresentationSize = 12;
    public const int FieldConditionOffset = 0;
    public const int FieldDevnodeOffset = 4;
    public const int FieldProblemOffset = 8;
    public const int Layout = 1;
    public const int BeatMs = 500;

    public static void Initialize(Span<byte> block)
    {
        Span<byte> whole = Whole(block);
        whole.Clear();
        BinaryPrimitives.WriteUInt32LittleEndian(whole[LayoutOffset..], Layout);
    }

    public static void Write(Span<byte> block, PadReport found)
    {
        Span<byte> whole = Whole(block);
        uint seq = Volatile.Read(ref Word(whole, SeqOffset));
        Volatile.Write(ref Word(whole, SeqOffset), seq + 1);
        Thread.MemoryBarrier();
        BinaryPrimitives.WriteUInt32LittleEndian(whole[PresentsOffset..], (uint)found.Presents);
        Put(whole, HidOffset, found.Hid);
        Put(whole, XinputOffset, found.Xinput);
        Put(whole, AudioOffset, found.Audio);
        Thread.MemoryBarrier();
        Volatile.Write(ref Word(whole, SeqOffset), seq + 2);
    }

    public static void Beat(Span<byte> block)
    {
        Span<byte> whole = Whole(block);
        Interlocked.Increment(ref Unsafe.As<uint, int>(ref Word(whole, HeartbeatOffset)));
    }

    // What satellite.exe reads, for the selftest: the heartbeat, the sequence,
    // and the last finished write, or none while one is under way.
    public readonly record struct Reading(uint Heartbeat, uint Seq, PadReport? Report);

    public static Reading Read(ReadOnlySpan<byte> block)
    {
        ReadOnlySpan<byte> whole = Whole(block);
        uint heartbeat = Load(whole, HeartbeatOffset);
        for (int attempt = 0; attempt < ReadAttempts; attempt++)
        {
            uint before = Load(whole, SeqOffset);
            if ((before & 1) != 0) continue;
            Thread.MemoryBarrier();
            var found = new PadReport((Presents)Load(whole, PresentsOffset), Take(whole, HidOffset),
                                      Take(whole, XinputOffset), Take(whole, AudioOffset));
            Thread.MemoryBarrier();
            if (Load(whole, SeqOffset) == before) return new Reading(heartbeat, before, found);
        }
        return new Reading(heartbeat, Load(whole, SeqOffset), null);
    }

    private const int ReadAttempts = 4;

    private static void Put(Span<byte> block, int offset, PresentationCondition part)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(block[(offset + FieldConditionOffset)..], (uint)part.Condition);
        BinaryPrimitives.WriteUInt32LittleEndian(block[(offset + FieldDevnodeOffset)..], (uint)part.Devnode);
        BinaryPrimitives.WriteUInt32LittleEndian(block[(offset + FieldProblemOffset)..], part.Problem);
    }

    private static PresentationCondition Take(ReadOnlySpan<byte> block, int offset) =>
        new((DevnodeCondition)Load(block, offset + FieldConditionOffset),
            (WatchedDevnode)Load(block, offset + FieldDevnodeOffset), Load(block, offset + FieldProblemOffset));

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
