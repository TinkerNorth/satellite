// SPDX-License-Identifier: LGPL-3.0-or-later
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class PadDevnodesTests
{
    private const uint Started = DevnodeStatus.Started;
    private static readonly DevnodeState Gone = default;

    private static DevnodeState Node(uint handle, uint status, uint problem = 0) =>
        new(true, status, problem, handle);

    private static DevnodeState Problem(uint handle, uint problem) =>
        Node(handle, DevnodeStatus.HasProblem, problem);

    // A device tree held in a test: instance ids to nodes, and each node's
    // children with their setup class.
    private sealed class FakeTree : IDevnodeTree
    {
        public Dictionary<string, DevnodeState> Located { get; } = new(StringComparer.OrdinalIgnoreCase);
        public Dictionary<uint, List<(DevnodeState Node, Guid Class)>> Children { get; } = new();

        public DevnodeState Locate(string instanceId) =>
            Located.TryGetValue(instanceId, out DevnodeState node) ? node : Gone;

        public DevnodeState FirstChild(DevnodeState parent) =>
            Children.TryGetValue(parent.Handle, out var list) && list.Count > 0 ? list[0].Node : Gone;

        public DevnodeState ChildOfClass(DevnodeState parent, Guid setupClass) =>
            Children.TryGetValue(parent.Handle, out var list)
                ? list.FirstOrDefault(c => c.Class == setupClass).Node
                : Gone;

        public void Child(DevnodeState parent, DevnodeState child, Guid setupClass = default)
        {
            if (!Children.TryGetValue(parent.Handle, out var list)) Children[parent.Handle] = list = new();
            list.Add((child, setupClass));
        }
    }

    private static readonly string Token = IdentityTags.TokenOf(SatelliteProfiles.IdentityKey(3, "dualsense"));
    private static readonly PadDevnodes Sony = DevnodeIds.For(0x054C, 0x0CE6, companion: false, usbip: false, Token);
    private static readonly PadDevnodes Xbox = DevnodeIds.For(0x045E, 0x028E, companion: true, usbip: false, Token);
    private static readonly PadDevnodes Composite = DevnodeIds.For(0x054C, 0x0CE6, companion: false, usbip: true, Token);

    // A UMDF2 Sony pad whose parent and HID child are both started.
    private static FakeTree WholeSony(out DevnodeState parent, out DevnodeState child)
    {
        var tree = new FakeTree();
        parent = Node(1, Started);
        child = Node(2, Started);
        tree.Located[Sony.Device] = parent;
        tree.Child(parent, child);
        return tree;
    }

    // A composite: its USB device, an audio function, the HID function and the
    // HID collection below that.
    private static FakeTree WholeComposite(out DevnodeState usb, out DevnodeState function, out DevnodeState audio)
    {
        var tree = new FakeTree();
        usb = Node(10, Started);
        audio = Node(11, Started);
        function = Node(12, Started);
        tree.Located[Composite.Device] = usb;
        tree.Child(usb, audio, Guid.NewGuid());
        tree.Child(usb, function, DevnodeIds.HidClass);
        tree.Child(function, Node(13, Started));
        return tree;
    }

    [Fact]
    public void ADevnodeIsClassifiedByWhatCmGetDevNodeStatusSays()
    {
        Assert.Equal(DevnodeCondition.Missing, DevnodeStatus.Classify(Gone));
        Assert.Equal(DevnodeCondition.Present, DevnodeStatus.Classify(Node(1, Started)));
        Assert.Equal(DevnodeCondition.Disabled, DevnodeStatus.Classify(Problem(1, DevnodeStatus.ProblemDisabled)));
        Assert.Equal(DevnodeCondition.Failed, DevnodeStatus.Classify(Problem(1, 43)));
        Assert.Equal(DevnodeCondition.Failed, DevnodeStatus.Classify(Node(1, Started | DevnodeStatus.PrivateProblem)));
        Assert.Equal(DevnodeCondition.Leaving, DevnodeStatus.Classify(Node(1, Started | DevnodeStatus.WillBeRemoved)));
        Assert.Equal(DevnodeCondition.Starting, DevnodeStatus.Classify(Node(1, 0)));
    }

    [Fact]
    public void AProblemOutranksARemovalUnderWay()
    {
        Assert.Equal(DevnodeCondition.Disabled,
                     DevnodeStatus.Classify(Node(1, DevnodeStatus.HasProblem | DevnodeStatus.WillBeRemoved, 22)));
    }

    [Fact]
    public void APlainHidPadsParentIsRootHidClassUnderItsToken()
    {
        Assert.Equal(@"ROOT\HIDClass\" + Token, Sony.Device);
        Assert.Null(Sony.Companion);
        Assert.False(Sony.Usbip);
    }

    [Fact]
    public void AnXusbCompanionPadsParentCarriesItsIdsAndItsCompanionIsSwdHidMaestro()
    {
        Assert.Equal(@"ROOT\VID_045E&PID_028E&IG_00\" + Token, Xbox.Device);
        Assert.Equal(@"SWD\HIDMAESTRO\" + Token, Xbox.Companion);
    }

    [Fact]
    public void ACompositesUsbDeviceIsNamedByItsSyntheticSerialTheTokensLow48Bits()
    {
        Assert.Equal(@"USB\VID_054C&PID_0CE6\HM" + Token[^12..], Composite.Device);
        Assert.Equal("HM" + Token[^12..], DevnodeIds.UsbSerial(Token));
        Assert.True(Composite.Usbip);
        Assert.Null(Composite.Companion);
    }

    [Fact]
    public void ATokenThatIsNotAnIdentityTokenIsRefused()
    {
        Assert.Throws<ArgumentException>(() => DevnodeIds.UsbSerial("HM_0000"));
        Assert.Throws<ArgumentException>(() => DevnodeIds.UsbSerial("XX_0123456789ABCDEF"));
    }

    [Fact]
    public void APadWhoseDevnodesAreAllStartedIsPresent()
    {
        FakeTree tree = WholeSony(out _, out _);
        Assert.Equal(PadCondition.Present, PadEvaluation.Evaluate(tree, Sony));
        FakeTree composite = WholeComposite(out _, out _, out _);
        Assert.Equal(PadCondition.Present, PadEvaluation.Evaluate(composite, Composite));
    }

    [Fact]
    public void AMissingParentIsTheDevicesAndItsChildIsNotLookedFor()
    {
        var tree = new FakeTree();
        Assert.Equal(new PadCondition(DevnodeCondition.Missing, WatchedDevnode.Device, 0), PadEvaluation.Evaluate(tree, Sony));
    }

    [Fact]
    public void AParentWithNoChildIsMissingItsHidCollection()
    {
        var tree = new FakeTree();
        tree.Located[Sony.Device] = Node(1, Started);
        Assert.Equal(new PadCondition(DevnodeCondition.Missing, WatchedDevnode.HidCollection, 0), PadEvaluation.Evaluate(tree, Sony));
    }

    [Fact]
    public void AFailedChildNamesItsProblem()
    {
        FakeTree tree = WholeSony(out DevnodeState parent, out _);
        tree.Children[parent.Handle][0] = (Problem(2, 43), default);
        Assert.Equal(new PadCondition(DevnodeCondition.Failed, WatchedDevnode.HidCollection, 43), PadEvaluation.Evaluate(tree, Sony));
    }

    [Fact]
    public void ADisabledParentIsDisabledThoughItsChildIsGone()
    {
        var tree = new FakeTree();
        tree.Located[Sony.Device] = Problem(1, 22);
        Assert.Equal(new PadCondition(DevnodeCondition.Disabled, WatchedDevnode.Device, 22), PadEvaluation.Evaluate(tree, Sony));
    }

    [Fact]
    public void ADisabledDevnodeOutranksAMissingOne()
    {
        var tree = new FakeTree();
        tree.Located[Xbox.Device] = Node(1, Started);
        tree.Located[Xbox.Companion!] = Problem(3, 22);
        Assert.Equal(new PadCondition(DevnodeCondition.Disabled, WatchedDevnode.Companion, 22), PadEvaluation.Evaluate(tree, Xbox));
    }

    [Fact]
    public void AnXusbPadNeedsItsCompanionToo()
    {
        var tree = new FakeTree();
        DevnodeState parent = Node(1, Started);
        tree.Located[Xbox.Device] = parent;
        tree.Child(parent, Node(2, Started));
        Assert.Equal(new PadCondition(DevnodeCondition.Missing, WatchedDevnode.Companion, 0), PadEvaluation.Evaluate(tree, Xbox));
        tree.Located[Xbox.Companion!] = Node(3, Started);
        Assert.Equal(PadCondition.Present, PadEvaluation.Evaluate(tree, Xbox));
    }

    [Fact]
    public void ACompositesHidFunctionIsTheChildOfTheHidSetupClass()
    {
        FakeTree tree = WholeComposite(out DevnodeState usb, out _, out _);
        tree.Children[usb.Handle].RemoveAt(1);
        Assert.Equal(new PadCondition(DevnodeCondition.Missing, WatchedDevnode.HidFunction, 0), PadEvaluation.Evaluate(tree, Composite));
    }

    [Fact]
    public void ACompositesDisabledAudioFunctionIsNotItsHid()
    {
        FakeTree tree = WholeComposite(out DevnodeState usb, out _, out _);
        var audio = tree.Children[usb.Handle][0];
        tree.Children[usb.Handle][0] = (Problem(11, 22), audio.Class);
        Assert.Equal(PadCondition.Present, PadEvaluation.Evaluate(tree, Composite));
    }

    [Fact]
    public void ACompositesDisabledHidFunctionIsDisabled()
    {
        FakeTree tree = WholeComposite(out DevnodeState usb, out _, out _);
        tree.Children[usb.Handle][1] = (Problem(12, 22), DevnodeIds.HidClass);
        Assert.Equal(new PadCondition(DevnodeCondition.Disabled, WatchedDevnode.HidFunction, 22), PadEvaluation.Evaluate(tree, Composite));
    }

    [Fact]
    public void ACompositesHidFunctionWithNoCollectionIsMissingIt()
    {
        FakeTree tree = WholeComposite(out _, out DevnodeState function, out _);
        tree.Children.Remove(function.Handle);
        Assert.Equal(new PadCondition(DevnodeCondition.Missing, WatchedDevnode.HidCollection, 0), PadEvaluation.Evaluate(tree, Composite));
    }

    [Fact]
    public void ACompositeWhoseUsbDeviceIsGoneIsMissingItsDevice()
    {
        var tree = new FakeTree();
        Assert.Equal(new PadCondition(DevnodeCondition.Missing, WatchedDevnode.Device, 0), PadEvaluation.Evaluate(tree, Composite));
    }
}
