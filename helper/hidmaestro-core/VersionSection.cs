// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Buffers.Binary;

namespace Satellite.HmHelper.Core;

// Global\HIDMaestroVersion<N> (HIDMAESTRO_VERSION_INFO, driver/hm_contract.h:47-65
// at v2.1.0), read for the selftest. satellite.exe reads it through
// src/core/hidmaestro_version.h, whose test holds these offsets to its own.
public readonly record struct VersionSection(
    int LayoutVersion, int SdkProtocolMin, int SdkProtocolMax, Version SdkVersion,
    int DriverProtocol, Version DriverVersion, int DriverCapabilities, int DriverProtocolMin,
    int CompanionProtocol, int CompanionProtocolMin, ulong IdentityTag,
    int DriverConfigError, int CompanionConfigError)
{
    public const int Size = 64;
    public const int LayoutOffset = 0;
    public const int SdkProtocolMinOffset = 4;
    public const int SdkProtocolMaxOffset = 8;
    public const int SdkVersionOffset = 12;
    public const int DriverProtocolOffset = 20;
    public const int DriverVersionOffset = 24;
    public const int DriverCapabilitiesOffset = 32;
    public const int DriverProtocolMinOffset = 36;
    public const int CompanionProtocolOffset = 40;
    public const int CompanionProtocolMinOffset = 44;
    public const int IdentityTagOffset = 48;
    public const int DriverConfigErrorOffset = 56;
    public const int CompanionConfigErrorOffset = 60;

    public static VersionSection Parse(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length < Size)
            throw new ArgumentException($"a version section is {Size} bytes, not {bytes.Length}", nameof(bytes));
        return new VersionSection(
            I32(bytes, LayoutOffset), I32(bytes, SdkProtocolMinOffset), I32(bytes, SdkProtocolMaxOffset),
            Release(bytes, SdkVersionOffset), I32(bytes, DriverProtocolOffset), Release(bytes, DriverVersionOffset),
            I32(bytes, DriverCapabilitiesOffset), I32(bytes, DriverProtocolMinOffset),
            I32(bytes, CompanionProtocolOffset), I32(bytes, CompanionProtocolMinOffset),
            BinaryPrimitives.ReadUInt64LittleEndian(bytes[IdentityTagOffset..]),
            I32(bytes, DriverConfigErrorOffset), I32(bytes, CompanionConfigErrorOffset));
    }

    private static int I32(ReadOnlySpan<byte> bytes, int offset) => BinaryPrimitives.ReadInt32LittleEndian(bytes[offset..]);

    private static ushort U16(ReadOnlySpan<byte> bytes, int offset) => BinaryPrimitives.ReadUInt16LittleEndian(bytes[offset..]);

    private static Version Release(ReadOnlySpan<byte> bytes, int offset) =>
        new(U16(bytes, offset), U16(bytes, offset + 2), U16(bytes, offset + 4), U16(bytes, offset + 6));
}
