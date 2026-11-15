// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using Microsoft.Win32.SafeHandles;
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

[SupportedOSPlatform("windows")]
public sealed class OverlappedIoTests
{
    private static readonly TimeSpan Budget = TimeSpan.FromSeconds(5);

    private sealed class Pipe : IDisposable
    {
        private const uint GenericRead = 0x80000000;
        private const uint GenericWrite = 0x40000000;
        private const uint OpenExisting = 3;
        private const uint FileFlagOverlapped = 0x40000000;

        public NamedPipeServerStream Server { get; }
        public SafeFileHandle Client { get; }

        public Pipe()
        {
            string name = "satellite-overlapped-io-" + Guid.NewGuid().ToString("N");
            Server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous,
                                               64 * 1024, 64 * 1024);
            Task accept = Server.WaitForConnectionAsync();
            Client = CreateFileW(@"\\.\pipe\" + name, GenericRead | GenericWrite, 0, 0, OpenExisting, FileFlagOverlapped, 0);
            if (Client.IsInvalid) throw new IOException($"could not open the pipe's client end (error {Marshal.GetLastWin32Error()})");
            accept.Wait(5000);
        }

        public void Dispose()
        {
            Client.Dispose();
            Server.Dispose();
        }

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern SafeFileHandle CreateFileW(string path, uint access, uint share, nint security,
                                                         uint disposition, uint flags, nint template);
    }

    [Fact]
    public void BytesWrittenWithinTheBudgetAreRead()
    {
        if (!OperatingSystem.IsWindows()) return;
        using var pipe = new Pipe();
        byte[] sent = { 1, 2, 3, 4, 5 };
        var writer = new Thread(() =>
        {
            Thread.Sleep(100);
            pipe.Server.Write(sent, 0, sent.Length);
            pipe.Server.Flush();
        });
        writer.Start();

        var buffer = new byte[64];
        int? read = OverlappedIo.Read(pipe.Client, buffer, Budget);
        writer.Join();

        Assert.Equal(sent.Length, read);
        Assert.Equal(sent, buffer.Take(sent.Length).ToArray());
    }

    [Fact]
    public void NothingWithinTheBudgetReadsNothingAndTheHandleStillReads()
    {
        if (!OperatingSystem.IsWindows()) return;
        using var pipe = new Pipe();
        var buffer = new byte[64];
        var clock = Stopwatch.StartNew();

        Assert.Null(OverlappedIo.Read(pipe.Client, buffer, TimeSpan.FromMilliseconds(200)));
        Assert.InRange(clock.ElapsedMilliseconds, 150, 4000);

        pipe.Server.Write(new byte[] { 9 }, 0, 1);
        pipe.Server.Flush();
        Assert.Equal(1, OverlappedIo.Read(pipe.Client, buffer, Budget));
        Assert.Equal(9, buffer[0]);
    }

    [Fact]
    public void BytesAlreadyWaitingAreReadAtOnce()
    {
        if (!OperatingSystem.IsWindows()) return;
        using var pipe = new Pipe();
        pipe.Server.Write(new byte[] { 7, 7 }, 0, 2);
        pipe.Server.Flush();

        var buffer = new byte[2];
        Assert.Equal(2, OverlappedIo.Read(pipe.Client, buffer, Budget));
        Assert.Equal(new byte[] { 7, 7 }, buffer);
    }

    [Fact]
    public void AnEndedConnectionIsAnError()
    {
        if (!OperatingSystem.IsWindows()) return;
        using var pipe = new Pipe();
        pipe.Server.Dispose();

        Assert.Throws<IOException>(() => OverlappedIo.Read(pipe.Client, new byte[8], Budget));
    }
}
