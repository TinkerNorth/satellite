// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Buffers.Binary;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace Satellite.HmHelper.Core;

// The tag the SDK writes into a controller's version section and its drivers
// compare with their devnode's: HIDMaestro v2.1.0's DeviceIdentity is internal,
// so its derivation is restated here (DeviceIdentity.cs:94, 115-130, 174-192,
// 272-276). Keyed identities only: Satellite never passes the default form.
public static class IdentityTags
{
    private const string TokenPrefix = "HM_";
    private const string HashDomain = "HIDMaestro.DeviceIdentity.v1\n";
    private const ulong FnvOffsetBasis = 0xCBF29CE484222325UL;
    private const ulong FnvPrime = 0x100000001B3UL;

    public static ulong OfKey(string key) => OfToken(TokenOf(key));

    public static string TokenOf(string key)
    {
        string trimmed = key.Trim();
        string hashed = trimmed.Length > 30 ? trimmed[..30] : trimmed;
        byte[] digest = SHA256.HashData(Encoding.UTF8.GetBytes(HashDomain + hashed));
        ulong hash = BinaryPrimitives.ReadUInt64BigEndian(digest);
        return TokenPrefix + hash.ToString("X16", CultureInfo.InvariantCulture);
    }

    public static ulong OfToken(string token)
    {
        ulong hash = FnvOffsetBasis;
        foreach (char unit in token)
        {
            hash = (hash ^ (byte)unit) * FnvPrime;
            hash = (hash ^ (byte)(unit >> 8)) * FnvPrime;
        }
        return hash;
    }
}
