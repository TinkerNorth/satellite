// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Runtime.InteropServices;

namespace Satellite.HmHelper;

internal static class NativeMethods
{
    public const uint FILE_MAP_READ = 0x0004;
    public const uint FILE_MAP_WRITE = 0x0002;
    public const uint EVENT_MODIFY_STATE = 0x0002;
    public const uint SYNCHRONIZE = 0x00100000;
    public const uint PROCESS_DUP_HANDLE = 0x0040;
    public const uint DUPLICATE_SAME_ACCESS = 0x0002;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern nint OpenFileMappingW(uint access, bool inherit, string name);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern nint OpenEventW(uint access, bool inherit, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern nint MapViewOfFile(nint mapping, uint access, uint offsetHigh, uint offsetLow, nuint bytes);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool UnmapViewOfFile(nint view);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern nint OpenProcess(uint access, bool inherit, int pid);

    [DllImport("kernel32.dll")]
    public static extern nint GetCurrentProcess();

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DuplicateHandle(nint sourceProcess, nint sourceHandle, nint targetProcess,
                                              out nint targetHandle, uint access, bool inherit, uint options);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(nint handle);
}
