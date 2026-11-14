// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Diagnostics;
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class DisposalsTests
{
    private static readonly TimeSpan Prompt = TimeSpan.FromMilliseconds(500);
    private static readonly TimeSpan Generous = TimeSpan.FromSeconds(10);

    // A removal that runs until the test lets it finish.
    private sealed class SlowRemoval
    {
        private readonly ManualResetEventSlim _release = new(false);
        public ManualResetEventSlim Started { get; } = new(false);
        public bool Finished { get; private set; }

        public void Run()
        {
            Started.Set();
            _release.Wait(Generous);
            Finished = true;
        }

        public void Finish() => _release.Set();
    }

    // `action` on a thread of its own; the event is set once it returns.
    private static ManualResetEventSlim OnAnotherThread(Action action)
    {
        var done = new ManualResetEventSlim(false);
        new Thread(() => { action(); done.Set(); }) { IsBackground = true }.Start();
        return done;
    }

    private static TimeSpan Timed(Action action)
    {
        var clock = Stopwatch.StartNew();
        action();
        return clock.Elapsed;
    }

    [Fact]
    public void AnUnplugIsAnsweredWhileItsRemovalIsStillRunning()
    {
        var disposals = new Disposals();
        var removal = new SlowRemoval();
        TimeSpan took = Timed(() => disposals.Begin(2, removal.Run));
        Assert.True(took < Prompt, $"Begin took {took.TotalMilliseconds} ms");
        Assert.True(removal.Started.Wait(Generous));
        Assert.False(removal.Finished);
        removal.Finish();
        disposals.AwaitAll();
        Assert.True(removal.Finished);
    }

    [Fact]
    public void APlugOfTheSameSerialWaitsForItsRemoval()
    {
        var disposals = new Disposals();
        var removal = new SlowRemoval();
        disposals.Begin(2, removal.Run);
        Assert.True(removal.Started.Wait(Generous));
        ManualResetEventSlim plug = OnAnotherThread(() => disposals.Await(2));
        Assert.False(plug.Wait(Prompt));
        removal.Finish();
        Assert.True(plug.Wait(Generous));
        Assert.True(removal.Finished);
    }

    [Fact]
    public void APlugOfAnotherSerialDoesNotWait()
    {
        var disposals = new Disposals();
        var removal = new SlowRemoval();
        disposals.Begin(2, removal.Run);
        Assert.True(removal.Started.Wait(Generous));
        TimeSpan took = Timed(() => disposals.Await(3));
        Assert.True(took < Prompt, $"Await(3) took {took.TotalMilliseconds} ms");
        removal.Finish();
        disposals.AwaitAll();
    }

    [Fact]
    public void TwoRemovalsOfOneSerialRunInTurn()
    {
        var disposals = new Disposals();
        var first = new SlowRemoval();
        var secondRan = new ManualResetEventSlim(false);
        bool firstWasDone = false;
        disposals.Begin(4, first.Run);
        disposals.Begin(4, () => { firstWasDone = first.Finished; secondRan.Set(); });
        Assert.True(first.Started.Wait(Generous));
        Assert.False(secondRan.Wait(Prompt));
        first.Finish();
        Assert.True(secondRan.Wait(Generous));
        Assert.True(firstWasDone);
    }

    [Fact]
    public void TheSessionsEndWaitsForEveryRemoval()
    {
        var disposals = new Disposals();
        var a = new SlowRemoval();
        var b = new SlowRemoval();
        disposals.Begin(1, a.Run);
        disposals.Begin(2, b.Run);
        Assert.True(a.Started.Wait(Generous) && b.Started.Wait(Generous));
        ManualResetEventSlim end = OnAnotherThread(disposals.AwaitAll);
        Assert.False(end.Wait(Prompt));
        a.Finish();
        b.Finish();
        Assert.True(end.Wait(Generous));
        Assert.True(a.Finished && b.Finished);
    }

    [Fact]
    public void ARemovalThatThrowsLeavesTheNextOneOfItsSerialToRun()
    {
        var disposals = new Disposals();
        bool ran = false;
        disposals.Begin(5, () => throw new InvalidOperationException("the SDK refused"));
        disposals.Begin(5, () => ran = true);
        disposals.Await(5);
        Assert.True(ran);
        disposals.AwaitAll();
    }
}
