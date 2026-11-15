// SPDX-License-Identifier: LGPL-3.0-or-later
namespace Satellite.HmHelper.Core;

// The controllers a session is removing, each on a thread of its own, so an
// unplug is answered at once however long the SDK's removal takes: against a
// program that holds a pad open, a removal query waits about 5.4 s before
// Windows refuses it and a driver that does not answer adds 2 s, several times
// over (the SDK's DeviceManager.cs, IRefusalMemory, at v2.1.0). The pipe stays
// free for every other pad meanwhile. A plug of the same serial waits for its
// removal first, since the same identity key names the same devnodes.
public sealed class Disposals
{
    private readonly object _lock = new();
    private readonly Dictionary<uint, Task> _pending = new();

    // Starts removing what `dispose` removes, after any removal of the same
    // serial still running, and returns at once. A removal that throws is
    // reported and ends; the next one of its serial still runs.
    public void Begin(uint serial, Action dispose)
    {
        lock (_lock)
        {
            Task before = _pending.TryGetValue(serial, out Task? running) ? running : Task.CompletedTask;
            _pending[serial] = before.ContinueWith(_ => Run(serial, dispose), CancellationToken.None,
                                                   TaskContinuationOptions.LongRunning, TaskScheduler.Default);
        }
    }

    // Returns once no removal of `serial` is running.
    public void Await(uint serial)
    {
        Task? running;
        lock (_lock)
        {
            if (!_pending.TryGetValue(serial, out running)) return;
        }
        running.Wait();
        lock (_lock)
        {
            if (_pending.TryGetValue(serial, out Task? now) && now == running) _pending.Remove(serial);
        }
    }

    // Returns once no removal is running.
    public void AwaitAll()
    {
        uint[] serials;
        lock (_lock)
        {
            serials = _pending.Keys.ToArray();
        }
        foreach (uint serial in serials) Await(serial);
    }

    private static void Run(uint serial, Action dispose)
    {
        try
        {
            dispose();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"satellite-hm-helper: removing serial {serial} failed: {ex.Message}");
        }
    }
}
