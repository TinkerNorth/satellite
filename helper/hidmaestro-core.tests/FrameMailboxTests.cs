// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Buffers.Binary;
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class FrameMailboxTests
{
    private static byte[] Section() => new byte[FrameMailbox.SectionSize];

    private static void Publish(byte[] section, uint seq, ReadOnlySpan<byte> data)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(section.AsSpan(FrameMailbox.SeqNoOffset), seq + 1);
        BinaryPrimitives.WriteUInt32LittleEndian(section.AsSpan(FrameMailbox.DataSizeOffset), (uint)data.Length);
        data.CopyTo(section.AsSpan(FrameMailbox.DataOffset));
        BinaryPrimitives.WriteUInt32LittleEndian(section.AsSpan(FrameMailbox.SeqNoOffset), seq + 2);
    }

    [Fact]
    public void TheLayoutIsTheDriverInputSection()
    {
        Assert.Equal(0, FrameMailbox.SeqNoOffset);
        Assert.Equal(4, FrameMailbox.DataSizeOffset);
        Assert.Equal(8, FrameMailbox.DataOffset);
        Assert.Equal(256, FrameMailbox.DataCapacity);
        Assert.Equal(362, FrameMailbox.SectionSize);
    }

    [Fact]
    public void NothingPublishedReadsNothing()
    {
        byte[] section = Section();
        uint lastSeq = 0;
        Assert.False(FrameMailbox.TryRead(section, ref lastSeq, new byte[FrameMailbox.DataCapacity], out int length));
        Assert.Equal(0, length);
        Assert.Equal(0u, lastSeq);
    }

    [Fact]
    public void AFrameIsReadOnceAndThenNotAgain()
    {
        byte[] section = Section();
        byte[] block = Enumerable.Range(1, 48).Select(i => (byte)i).ToArray();
        Publish(section, 0, block);
        var frame = new byte[FrameMailbox.DataCapacity];
        uint lastSeq = 0;

        Assert.True(FrameMailbox.TryRead(section, ref lastSeq, frame, out int length));
        Assert.Equal(48, length);
        Assert.Equal(block, frame.Take(48).ToArray());
        Assert.Equal(2u, lastSeq);

        Assert.False(FrameMailbox.TryRead(section, ref lastSeq, frame, out length));
        Assert.Equal(0, length);
    }

    [Fact]
    public void TheNewestFrameWins()
    {
        byte[] section = Section();
        Publish(section, 0, new byte[] { 1, 1, 1 });
        Publish(section, 2, new byte[] { 2, 2 });
        var frame = new byte[FrameMailbox.DataCapacity];
        uint lastSeq = 0;

        Assert.True(FrameMailbox.TryRead(section, ref lastSeq, frame, out int length));
        Assert.Equal(2, length);
        Assert.Equal(new byte[] { 2, 2 }, frame.Take(2).ToArray());
        Assert.Equal(4u, lastSeq);
    }

    [Fact]
    public void AWriteInProgressIsNotAFrame()
    {
        byte[] section = Section();
        Publish(section, 0, new byte[] { 9 });
        BinaryPrimitives.WriteUInt32LittleEndian(section.AsSpan(FrameMailbox.SeqNoOffset), 3);
        var frame = new byte[FrameMailbox.DataCapacity];
        uint lastSeq = 0;

        Assert.False(FrameMailbox.TryRead(section, ref lastSeq, frame, out _));
        Assert.Equal(0u, lastSeq);
    }

    [Fact]
    public void AnOversizedDataSizeIsClampedToTheCapacity()
    {
        byte[] section = Section();
        Publish(section, 0, new byte[FrameMailbox.DataCapacity]);
        BinaryPrimitives.WriteUInt32LittleEndian(section.AsSpan(FrameMailbox.DataSizeOffset), 999);
        uint lastSeq = 0;

        Assert.True(FrameMailbox.TryRead(section, ref lastSeq, new byte[FrameMailbox.DataCapacity], out int length));
        Assert.Equal(FrameMailbox.DataCapacity, length);
    }

    [Fact]
    public void AnEmptyFrameIsAFrame()
    {
        byte[] section = Section();
        Publish(section, 0, ReadOnlySpan<byte>.Empty);
        uint lastSeq = 0;

        Assert.True(FrameMailbox.TryRead(section, ref lastSeq, new byte[FrameMailbox.DataCapacity], out int length));
        Assert.Equal(0, length);
        Assert.Equal(2u, lastSeq);
    }

    [Fact]
    public void ASectionOrBufferTooSmallIsRefused()
    {
        uint lastSeq = 0;
        Assert.Throws<ArgumentException>(() =>
            FrameMailbox.TryRead(new byte[FrameMailbox.SectionSize - 1], ref lastSeq, new byte[FrameMailbox.DataCapacity], out _));
        Assert.Throws<ArgumentException>(() =>
            FrameMailbox.TryRead(Section(), ref lastSeq, new byte[FrameMailbox.DataCapacity - 1], out _));
    }

    [Fact]
    public void AReaderRacingTheWriterNeverSeesATornFrame()
    {
        byte[] section = Section();
        var stop = new ManualResetEventSlim(false);
        var writer = new Thread(() =>
        {
            uint seq = 0;
            var block = new byte[48];
            while (!stop.IsSet)
            {
                byte fill = (byte)((seq / 2 + 1) & 0xFF);
                Array.Fill(block, fill);
                BinaryPrimitives.WriteUInt32LittleEndian(section.AsSpan(FrameMailbox.SeqNoOffset), seq + 1);
                Thread.MemoryBarrier();
                BinaryPrimitives.WriteUInt32LittleEndian(section.AsSpan(FrameMailbox.DataSizeOffset), 48);
                block.CopyTo(section.AsSpan(FrameMailbox.DataOffset));
                Thread.MemoryBarrier();
                BinaryPrimitives.WriteUInt32LittleEndian(section.AsSpan(FrameMailbox.SeqNoOffset), seq + 2);
                seq += 2;
            }
        });
        writer.Start();

        var frame = new byte[FrameMailbox.DataCapacity];
        uint lastSeq = 0;
        int read = 0;
        int torn = 0;
        var deadline = DateTime.UtcNow.AddMilliseconds(300);
        while (DateTime.UtcNow < deadline)
        {
            if (!FrameMailbox.TryRead(section, ref lastSeq, frame, out int length)) continue;
            read++;
            byte expected = (byte)((lastSeq / 2) & 0xFF);
            if (length != 48 || frame.Take(48).Any(b => b != expected)) torn++;
        }
        stop.Set();
        writer.Join();

        Assert.True(read > 0);
        Assert.Equal(0, torn);
    }
}
