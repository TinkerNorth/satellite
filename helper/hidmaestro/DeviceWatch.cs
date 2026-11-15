// SPDX-License-Identifier: LGPL-3.0-or-later
using System.IO.MemoryMappedFiles;
using Satellite.HmHelper.Core;

namespace Satellite.HmHelper;

// One session's device watch: a block per pad, handed to satellite.exe
// read-only, which a watcher thread fills with what Windows has of the pad's
// devnodes and a heartbeat thread beats. The heartbeat makes no Plug and Play
// call and waits on no lock the watcher holds across one, so only the helper
// itself stopping can stop it. Polled rather than notified: Windows sends a
// devnode that is disabled no notification at all, only the children that
// leave with it (tests/test_windows_devnode_probe.cpp), so disabling a HID
// collection or an XUSB companion, which have none, would raise nothing; and
// every decision satellite.exe takes waits a grace of seconds or holds a
// device that is disabled anyway, so a look every half second is soon enough.
internal sealed class DeviceWatch : IDisposable
{
    private const int LookEveryMs = DeviceWatchBlock.BeatMs;
    // The block promises a beat at least every BeatMs.
    private const int BeatEveryMs = DeviceWatchBlock.BeatMs / 2;
    private const int JoinTimeoutMs = 2000;

    private readonly IDevnodeTree _tree;
    private readonly object _lock = new();
    private readonly Dictionary<uint, WatchedPad> _pads = new();
    private readonly ManualResetEvent _stop = new(false);
    private readonly Thread _watcher;
    private readonly Thread _heart;
    private bool _disposed;

    public DeviceWatch(IDevnodeTree tree)
    {
        _tree = tree;
        _watcher = new Thread(Watch) { IsBackground = true, Name = "hm-device-watch" };
        _heart = new Thread(Beat) { IsBackground = true, Name = "hm-device-beat" };
        _watcher.Start();
        _heart.Start();
    }

    public WatchedPad Add(uint serial, PadDevnodes devnodes, Func<nint, ulong> handOver)
    {
        var pad = WatchedPad.Create(devnodes, handOver);
        lock (_lock)
        {
            if (_pads.Remove(serial, out WatchedPad? old)) old.Dispose();
            _pads[serial] = pad;
        }
        return pad;
    }

    public void Remove(uint serial)
    {
        WatchedPad? pad;
        lock (_lock)
        {
            if (!_pads.Remove(serial, out pad)) return;
        }
        pad.Dispose();
    }

    private WatchedPad[] Pads()
    {
        lock (_lock)
        {
            return _pads.Values.ToArray();
        }
    }

    private void Watch()
    {
        do
        {
            foreach (WatchedPad pad in Pads()) pad.Look(_tree);
        } while (!_stop.WaitOne(LookEveryMs));
    }

    private void Beat()
    {
        while (!_stop.WaitOne(BeatEveryMs))
        {
            foreach (WatchedPad pad in Pads()) pad.Beat();
        }
    }

    public void Dispose()
    {
        lock (_lock)
        {
            if (_disposed) return;
            _disposed = true;
        }
        _stop.Set();
        _watcher.Join(JoinTimeoutMs);
        _heart.Join(JoinTimeoutMs);
        foreach (WatchedPad pad in Pads()) pad.Dispose();
        lock (_lock)
        {
            _pads.Clear();
        }
    }
}

// One pad's block. A look reads the device tree with no lock held and takes
// the pad's own lock only to write what it found, which is all a beat waits on.
internal sealed unsafe class WatchedPad : IDisposable
{
    private readonly PadDevnodes _devnodes;
    private readonly MemoryMappedFile _section;
    private readonly MemoryMappedViewAccessor _view;
    private readonly byte* _block;
    private readonly object _gate = new();
    private bool _disposed;
    private string? _failure;

    public ulong Handle { get; private set; }

    private WatchedPad(PadDevnodes devnodes)
    {
        _devnodes = devnodes;
        _section = MemoryMappedFile.CreateNew(null, DeviceWatchBlock.BlockSize);
        _view = _section.CreateViewAccessor(0, DeviceWatchBlock.BlockSize);
        byte* pointer = null;
        _view.SafeMemoryMappedViewHandle.AcquirePointer(ref pointer);
        _block = pointer + _view.PointerOffset;
        DeviceWatchBlock.Initialize(Span());
    }

    public static WatchedPad Create(PadDevnodes devnodes, Func<nint, ulong> handOver)
    {
        var pad = new WatchedPad(devnodes);
        try
        {
            pad.Handle = handOver(pad._section.SafeMemoryMappedFileHandle.DangerousGetHandle());
            return pad;
        }
        catch
        {
            pad.Dispose();
            throw;
        }
    }

    private Span<byte> Span() => new(_block, DeviceWatchBlock.BlockSize);

    public void Look(IDevnodeTree tree)
    {
        PadReport found;
        try
        {
            found = PadEvaluation.Evaluate(tree, _devnodes);
        }
        catch (Exception ex)
        {
            if (_failure != ex.Message) Console.Error.WriteLine($"satellite-hm-helper: the device watch could not look at {_devnodes.Device}: {ex.Message}");
            _failure = ex.Message;
            return;
        }
        lock (_gate)
        {
            if (!_disposed) DeviceWatchBlock.Write(Span(), found);
        }
    }

    public void Beat()
    {
        lock (_gate)
        {
            if (!_disposed) DeviceWatchBlock.Beat(Span());
        }
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
        }
        _view.SafeMemoryMappedViewHandle.ReleasePointer();
        _view.Dispose();
        _section.Dispose();
    }
}
