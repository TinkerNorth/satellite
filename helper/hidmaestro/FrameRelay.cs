// SPDX-License-Identifier: LGPL-3.0-or-later
using System.IO.MemoryMappedFiles;
using HIDMaestro;
using Satellite.HmHelper.Core;

namespace Satellite.HmHelper;

internal sealed unsafe class FrameRelay : IDisposable
{
    private const int DoorbellTimeoutMs = 500;
    private const int JoinTimeoutMs = 2000;

    private readonly HMController _controller;
    private readonly MemoryMappedFile _section;
    private readonly MemoryMappedViewAccessor _view;
    private readonly EventWaitHandle _doorbell;
    private readonly EventWaitHandle _cancel;
    private Thread? _thread;
    private volatile bool _disposed;

    public ulong SectionHandle { get; private set; }
    public ulong DoorbellHandle { get; private set; }

    private FrameRelay(HMController controller)
    {
        _controller = controller;
        _section = MemoryMappedFile.CreateNew(null, FrameMailbox.SectionSize);
        _view = _section.CreateViewAccessor(0, FrameMailbox.SectionSize);
        _doorbell = new EventWaitHandle(false, EventResetMode.AutoReset);
        _cancel = new EventWaitHandle(false, EventResetMode.ManualReset);
    }

    public static FrameRelay Start(HMController controller, Func<nint, ulong> duplicate)
    {
        var relay = new FrameRelay(controller);
        try
        {
            relay.SectionHandle = duplicate(relay._section.SafeMemoryMappedFileHandle.DangerousGetHandle());
            relay.DoorbellHandle = duplicate(relay._doorbell.SafeWaitHandle.DangerousGetHandle());
            if (relay.SectionHandle == 0 || relay.DoorbellHandle == 0)
                throw new InvalidOperationException("could not duplicate the frame mailbox handles");
            relay._thread = new Thread(relay.Loop) { IsBackground = true, Name = "hm-frame-relay" };
            relay._thread.Start();
            return relay;
        }
        catch
        {
            relay.Dispose();
            throw;
        }
    }

    private void Loop()
    {
        byte* pointer = null;
        _view.SafeMemoryMappedViewHandle.AcquirePointer(ref pointer);
        try
        {
            byte* start = pointer + _view.PointerOffset;
            var frame = new byte[FrameMailbox.DataCapacity];
            WaitHandle[] waits = { _cancel, _doorbell };
            uint lastSeq = 0;
            string? refused = null;
            while (!_disposed)
            {
                if (WaitHandle.WaitAny(waits, DoorbellTimeoutMs) == 0) return;
                var section = new ReadOnlySpan<byte>(start, FrameMailbox.SectionSize);
                while (!_disposed && FrameMailbox.TryRead(section, ref lastSeq, frame, out int length))
                {
                    try
                    {
                        _controller.SubmitRawReport(new ReadOnlySpan<byte>(frame, 0, length));
                    }
                    catch (ArgumentException ex)
                    {
                        if (refused == ex.Message) continue;
                        refused = ex.Message;
                        Console.Error.WriteLine($"satellite-hm-helper: a {length}-byte frame was refused: {ex.Message}");
                    }
                    catch (ObjectDisposedException)
                    {
                        return;
                    }
                }
            }
        }
        finally
        {
            _view.SafeMemoryMappedViewHandle.ReleasePointer();
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _cancel.Set();
        bool joined = true;
        if (_thread != null)
        {
            try { joined = _thread.Join(JoinTimeoutMs); }
            catch { joined = false; }
            _thread = null;
        }
        if (!joined) return;
        _view.Dispose();
        _section.Dispose();
        _doorbell.Dispose();
        _cancel.Dispose();
    }
}
