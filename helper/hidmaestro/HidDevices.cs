// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Satellite.HmHelper;

internal static class HidDevices
{
    private const uint DIGCF_PRESENT = 0x02;
    private const uint DIGCF_DEVICEINTERFACE = 0x10;
    private const uint GENERIC_READ = 0x80000000;
    private const uint GENERIC_WRITE = 0x40000000;
    private const uint FILE_SHARE_READ = 1;
    private const uint FILE_SHARE_WRITE = 2;
    private const uint OPEN_EXISTING = 3;

    public static IReadOnlyList<string> Paths(ushort vendorId, ushort productId)
    {
        var paths = new List<string>();
        HidD_GetHidGuid(out Guid hidGuid);
        nint set = SetupDiGetClassDevs(ref hidGuid, null, 0, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == -1) return paths;
        try
        {
            var iface = new SP_DEVICE_INTERFACE_DATA { cbSize = Marshal.SizeOf<SP_DEVICE_INTERFACE_DATA>() };
            for (uint i = 0; SetupDiEnumDeviceInterfaces(set, 0, ref hidGuid, i, ref iface); i++)
            {
                SetupDiGetDeviceInterfaceDetail(set, ref iface, 0, 0, out int needed, 0);
                if (needed <= 0) continue;
                nint detail = Marshal.AllocHGlobal(needed);
                try
                {
                    Marshal.WriteInt32(detail, nint.Size == 8 ? 8 : 6);
                    if (!SetupDiGetDeviceInterfaceDetail(set, ref iface, detail, needed, out _, 0)) continue;
                    string path = Marshal.PtrToStringUni(detail + 4) ?? "";
                    if (Attributes(path) is { } attrs && attrs.VendorID == vendorId && attrs.ProductID == productId)
                        paths.Add(path);
                }
                finally
                {
                    Marshal.FreeHGlobal(detail);
                }
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
        return paths;
    }

    public static string? WaitForNewPath(ushort vendorId, ushort productId, IReadOnlyList<string> before, TimeSpan budget)
    {
        DateTime deadline = DateTime.UtcNow + budget;
        while (true)
        {
            string? fresh = Paths(vendorId, productId).FirstOrDefault(p => !before.Contains(p, StringComparer.OrdinalIgnoreCase));
            if (fresh != null || DateTime.UtcNow >= deadline) return fresh;
            Thread.Sleep(100);
        }
    }

    public static byte[]? GetFeature(string path, byte reportId, int length)
    {
        using SafeFileHandle h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
        if (h.IsInvalid) return null;
        if (!HidD_GetPreparsedData(h, out nint prepared)) return null;
        try
        {
            HidP_GetCaps(prepared, out HIDP_CAPS caps);
            int size = Math.Max(caps.FeatureReportByteLength, length);
            var buffer = new byte[size];
            buffer[0] = reportId;
            return HidD_GetFeature(h, buffer, caps.FeatureReportByteLength) ? buffer : null;
        }
        finally
        {
            HidD_FreePreparsedData(prepared);
        }
    }

    private static HIDD_ATTRIBUTES? Attributes(string path)
    {
        using SafeFileHandle h = CreateFile(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
        if (h.IsInvalid) return null;
        var attrs = new HIDD_ATTRIBUTES { Size = Marshal.SizeOf<HIDD_ATTRIBUTES>() };
        return HidD_GetAttributes(h, ref attrs) ? attrs : null;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SP_DEVICE_INTERFACE_DATA
    {
        public int cbSize;
        public Guid InterfaceClassGuid;
        public int Flags;
        public nint Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct HIDD_ATTRIBUTES
    {
        public int Size;
        public ushort VendorID;
        public ushort ProductID;
        public ushort VersionNumber;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct HIDP_CAPS
    {
        public ushort Usage;
        public ushort UsagePage;
        public ushort InputReportByteLength;
        public ushort OutputReportByteLength;
        public ushort FeatureReportByteLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public ushort[] Reserved;
        public ushort NumberLinkCollectionNodes;
        public ushort NumberInputButtonCaps;
        public ushort NumberInputValueCaps;
        public ushort NumberInputDataIndices;
        public ushort NumberOutputButtonCaps;
        public ushort NumberOutputValueCaps;
        public ushort NumberOutputDataIndices;
        public ushort NumberFeatureButtonCaps;
        public ushort NumberFeatureValueCaps;
        public ushort NumberFeatureDataIndices;
    }

    [DllImport("hid.dll")]
    private static extern void HidD_GetHidGuid(out Guid guid);

    [DllImport("hid.dll", SetLastError = true)]
    private static extern bool HidD_GetAttributes(SafeFileHandle handle, ref HIDD_ATTRIBUTES attributes);

    [DllImport("hid.dll", SetLastError = true)]
    private static extern bool HidD_GetFeature(SafeFileHandle handle, byte[] buffer, int bufferLength);

    [DllImport("hid.dll", SetLastError = true)]
    private static extern bool HidD_GetPreparsedData(SafeFileHandle handle, out nint prepared);

    [DllImport("hid.dll")]
    private static extern bool HidD_FreePreparsedData(nint prepared);

    [DllImport("hid.dll")]
    private static extern int HidP_GetCaps(nint prepared, out HIDP_CAPS caps);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern nint SetupDiGetClassDevs(ref Guid guid, string? enumerator, nint parent, uint flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiEnumDeviceInterfaces(nint set, nint deviceInfo, ref Guid guid, uint index,
                                                           ref SP_DEVICE_INTERFACE_DATA data);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool SetupDiGetDeviceInterfaceDetail(nint set, ref SP_DEVICE_INTERFACE_DATA data, nint detail,
                                                               int detailSize, out int required, nint deviceInfo);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiDestroyDeviceInfoList(nint set);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern SafeFileHandle CreateFile(string path, uint access, uint share, nint security, uint disposition,
                                                    uint flags, nint template);
}
