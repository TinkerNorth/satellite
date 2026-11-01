// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Runtime.CompilerServices;

namespace Satellite.HmHelper.Core;

public static class FrameMailbox
{
    public const int SeqNoOffset = 0;
    public const int DataSizeOffset = 4;
    public const int DataOffset = 8;
    public const int DataCapacity = 256;
    public const int SectionSize = 362;
    public const int Retries = 4;

    public static bool TryRead(ReadOnlySpan<byte> section, ref uint lastSeq, Span<byte> frame, out int length)
    {
        length = 0;
        if (section.Length < SectionSize)
            throw new ArgumentException($"a frame mailbox is {SectionSize} bytes, not {section.Length}", nameof(section));
        if (frame.Length < DataCapacity)
            throw new ArgumentException($"a frame buffer holds {DataCapacity} bytes, not {frame.Length}", nameof(frame));

        for (int attempt = 0; attempt < Retries; attempt++)
        {
            uint before = ReadU32(section, SeqNoOffset);
            if (before == lastSeq) return false;
            if ((before & 1) != 0) continue;
            Thread.MemoryBarrier();
            int size = (int)Math.Min(ReadU32(section, DataSizeOffset), DataCapacity);
            section.Slice(DataOffset, size).CopyTo(frame);
            Thread.MemoryBarrier();
            if (ReadU32(section, SeqNoOffset) != before) continue;
            lastSeq = before;
            length = size;
            return true;
        }
        return false;
    }

    private static uint ReadU32(ReadOnlySpan<byte> section, int offset) =>
        Volatile.Read(ref Unsafe.As<byte, uint>(ref Unsafe.AsRef(in section[offset])));
}
