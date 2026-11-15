// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Threading;

namespace Satellite.HmHelper.Core;

[SupportedOSPlatform("windows")]
public static class OverlappedIo
{
    private const int ErrorIoPending = 997;
    private const int ErrorOperationAborted = 995;

    public static int? Read(SafeHandle handle, byte[] buffer, TimeSpan budget)
    {
        using var done = new ManualResetEvent(false);
        GCHandle pinned = GCHandle.Alloc(buffer, GCHandleType.Pinned);
        nint overlapped = Marshal.AllocHGlobal(Marshal.SizeOf<NativeOverlapped>());
        try
        {
            var state = new NativeOverlapped { EventHandle = done.SafeWaitHandle.DangerousGetHandle() };
            Marshal.StructureToPtr(state, overlapped, false);
            bool finished = ReadFile(handle, pinned.AddrOfPinnedObject(), buffer.Length, 0, overlapped);
            if (!finished)
            {
                int started = Marshal.GetLastWin32Error();
                if (started != ErrorIoPending) throw new IOException($"ReadFile failed with error {started}");
                if (!done.WaitOne(budget)) CancelIoEx(handle, overlapped);
            }
            if (GetOverlappedResult(handle, overlapped, out int moved, true)) return moved > 0 ? moved : null;
            int ended = Marshal.GetLastWin32Error();
            if (ended == ErrorOperationAborted) return null;
            throw new IOException($"the read ended with error {ended}");
        }
        finally
        {
            Marshal.FreeHGlobal(overlapped);
            pinned.Free();
        }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ReadFile(SafeHandle handle, nint buffer, int bytes, nint bytesRead, nint overlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetOverlappedResult(SafeHandle handle, nint overlapped, out int transferred, bool wait);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CancelIoEx(SafeHandle handle, nint overlapped);
}
