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
    private static readonly PadDevnodes Composite = DevnodeIds.For(0x054C, 0x0CE6, companion: false, usbip: true, Token,
                                                                   audio: true);
    private static readonly PadDevnodes SilentComposite = DevnodeIds.For(0x054C, 0x0CE6, companion: false, usbip: true,
                                                                         Token);

    private static PresentationCondition Is(DevnodeCondition condition, WatchedDevnode devnode, uint problem = 0) =>
        new(condition, devnode, problem);

    private static readonly PresentationCondition HidPresent = Is(DevnodeCondition.Present, WatchedDevnode.HidCollection);
    private static readonly PresentationCondition CompanionPresent = Is(DevnodeCondition.Present, WatchedDevnode.Companion);
    private static readonly PresentationCondition AudioPresent = Is(DevnodeCondition.Present, WatchedDevnode.AudioFunction);

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

    // A composite: its USB device, its audio function, the HID function and the
    // HID collection below that.
    private static FakeTree WholeComposite(out DevnodeState usb, out DevnodeState function, out DevnodeState audio)
    {
        var tree = new FakeTree();
        usb = Node(10, Started);
        audio = Node(11, Started);
        function = Node(12, Started);
        tree.Located[Composite.Device] = usb;
        tree.Child(usb, audio, DevnodeIds.MediaClass);
        tree.Child(usb, function, DevnodeIds.HidClass);
        tree.Child(function, Node(13, Started));
        return tree;
    }

    // An Xbox pad whose parent, HID child and XUSB companion are all started.
    private static FakeTree WholeXbox(out DevnodeState parent)
    {
        var tree = new FakeTree();
        parent = Node(1, Started);
        tree.Located[Xbox.Device] = parent;
        tree.Child(parent, Node(2, Started));
        tree.Located[Xbox.Companion!] = Node(3, Started);
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

    // devguid.h's GUID_DEVCLASS_HIDCLASS and GUID_DEVCLASS_MEDIA, the setup
    // classes a composite's HID and USB-audio functions install into.
    [Fact]
    public void TheSetupClassesAreWindowsOwn()
    {
        Assert.Equal(new Guid("745a17a0-74d3-11d0-b6fe-00a0c90f57da"), DevnodeIds.HidClass);
        Assert.Equal(new Guid("4d36e96c-e325-11ce-bfc1-08002be10318"), DevnodeIds.MediaClass);
    }

    [Fact]
    public void ATokenThatIsNotAnIdentityTokenIsRefused()
    {
        Assert.Throws<ArgumentException>(() => DevnodeIds.UsbSerial("HM_0000"));
        Assert.Throws<ArgumentException>(() => DevnodeIds.UsbSerial("XX_0123456789ABCDEF"));
    }

    [Fact]
    public void APadWhoseDevnodesAreAllStartedIsWhole()
    {
        Assert.Equal(new PadReport(Presents.Hid, HidPresent, default, default), PadEvaluation.Evaluate(WholeSony(out _, out _), Sony));
        Assert.Equal(new PadReport(Presents.Hid | Presents.Xinput, HidPresent, CompanionPresent, default),
                     PadEvaluation.Evaluate(WholeXbox(out _), Xbox));
        PadReport composite = PadEvaluation.Evaluate(WholeComposite(out _, out _, out _), Composite);
        Assert.Equal(new PadReport(Presents.Hid | Presents.Audio, HidPresent, default, AudioPresent), composite);
        Assert.True(composite.Whole);
    }

    [Fact]
    public void AMissingParentIsTheDevicesAndItsChildIsNotLookedFor()
    {
        Assert.Equal(Is(DevnodeCondition.Missing, WatchedDevnode.Device), PadEvaluation.Evaluate(new FakeTree(), Sony).Hid);
    }

    [Fact]
    public void AParentWithNoChildIsMissingItsHidCollection()
    {
        var tree = new FakeTree();
        tree.Located[Sony.Device] = Node(1, Started);
        Assert.Equal(Is(DevnodeCondition.Missing, WatchedDevnode.HidCollection), PadEvaluation.Evaluate(tree, Sony).Hid);
    }

    [Fact]
    public void AFailedChildNamesItsProblem()
    {
        FakeTree tree = WholeSony(out DevnodeState parent, out _);
        tree.Children[parent.Handle][0] = (Problem(2, 43), default);
        Assert.Equal(Is(DevnodeCondition.Failed, WatchedDevnode.HidCollection, 43), PadEvaluation.Evaluate(tree, Sony).Hid);
    }

    [Fact]
    public void ADisabledParentIsDisabledThoughItsChildIsGone()
    {
        var tree = new FakeTree();
        tree.Located[Sony.Device] = Problem(1, 22);
        Assert.Equal(Is(DevnodeCondition.Disabled, WatchedDevnode.Device, 22), PadEvaluation.Evaluate(tree, Sony).Hid);
    }

    [Fact]
    public void AnXboxPadsDisabledCompanionLeavesItsHidPresentationPresent()
    {
        FakeTree tree = WholeXbox(out _);
        tree.Located[Xbox.Companion!] = Problem(3, 22);
        PadReport report = PadEvaluation.Evaluate(tree, Xbox);
        Assert.Equal(HidPresent, report.Hid);
        Assert.Equal(Is(DevnodeCondition.Disabled, WatchedDevnode.Companion, 22), report.Xinput);
        Assert.False(report.Whole);
    }

    [Fact]
    public void AnXboxPadsDisabledHidChildLeavesItsCompanionPresent()
    {
        FakeTree tree = WholeXbox(out DevnodeState parent);
        tree.Children[parent.Handle][0] = (Problem(2, 22), default);
        PadReport report = PadEvaluation.Evaluate(tree, Xbox);
        Assert.Equal(Is(DevnodeCondition.Disabled, WatchedDevnode.HidCollection, 22), report.Hid);
        Assert.Equal(CompanionPresent, report.Xinput);
    }

    [Fact]
    public void AFailedParentBesideADisabledCompanionReportsBoth()
    {
        var tree = new FakeTree();
        tree.Located[Xbox.Device] = Problem(1, 43);
        tree.Located[Xbox.Companion!] = Problem(3, 22);
        PadReport report = PadEvaluation.Evaluate(tree, Xbox);
        Assert.Equal(Is(DevnodeCondition.Failed, WatchedDevnode.Device, 43), report.Hid);
        Assert.Equal(Is(DevnodeCondition.Disabled, WatchedDevnode.Companion, 22), report.Xinput);
    }

    [Fact]
    public void AnXboxPadsMissingCompanionIsItsXinputPresentations()
    {
        FakeTree tree = WholeXbox(out _);
        tree.Located.Remove(Xbox.Companion!);
        PadReport report = PadEvaluation.Evaluate(tree, Xbox);
        Assert.Equal(HidPresent, report.Hid);
        Assert.Equal(Is(DevnodeCondition.Missing, WatchedDevnode.Companion), report.Xinput);
    }

    [Fact]
    public void ACompositesHidFunctionIsTheChildOfTheHidSetupClass()
    {
        FakeTree tree = WholeComposite(out DevnodeState usb, out _, out _);
        tree.Children[usb.Handle].RemoveAt(1);
        PadReport report = PadEvaluation.Evaluate(tree, Composite);
        Assert.Equal(Is(DevnodeCondition.Missing, WatchedDevnode.HidFunction), report.Hid);
        Assert.Equal(AudioPresent, report.Audio);
    }

    [Fact]
    public void ACompositesAudioFunctionIsTheChildOfTheMediaSetupClass()
    {
        FakeTree tree = WholeComposite(out DevnodeState usb, out _, out _);
        tree.Children[usb.Handle].RemoveAt(0);
        PadReport report = PadEvaluation.Evaluate(tree, Composite);
        Assert.Equal(HidPresent, report.Hid);
        Assert.Equal(Is(DevnodeCondition.Missing, WatchedDevnode.AudioFunction), report.Audio);
    }

    [Fact]
    public void ACompositesDisabledAudioFunctionLeavesItsHidPresent()
    {
        FakeTree tree = WholeComposite(out DevnodeState usb, out _, out _);
        tree.Children[usb.Handle][0] = (Problem(11, 22), DevnodeIds.MediaClass);
        PadReport report = PadEvaluation.Evaluate(tree, Composite);
        Assert.Equal(HidPresent, report.Hid);
        Assert.Equal(Is(DevnodeCondition.Disabled, WatchedDevnode.AudioFunction, 22), report.Audio);
    }

    [Fact]
    public void ACompositesDisabledHidFunctionLeavesItsAudioPresent()
    {
        FakeTree tree = WholeComposite(out DevnodeState usb, out _, out _);
        tree.Children[usb.Handle][1] = (Problem(12, 22), DevnodeIds.HidClass);
        PadReport report = PadEvaluation.Evaluate(tree, Composite);
        Assert.Equal(Is(DevnodeCondition.Disabled, WatchedDevnode.HidFunction, 22), report.Hid);
        Assert.Equal(AudioPresent, report.Audio);
    }

    [Fact]
    public void ACompositesHidFunctionWithNoCollectionIsMissingIt()
    {
        FakeTree tree = WholeComposite(out _, out DevnodeState function, out _);
        tree.Children.Remove(function.Handle);
        Assert.Equal(Is(DevnodeCondition.Missing, WatchedDevnode.HidCollection), PadEvaluation.Evaluate(tree, Composite).Hid);
    }

    [Fact]
    public void ACompositeWhoseUsbDeviceIsGoneIsMissingBothItsPresentations()
    {
        PadReport report = PadEvaluation.Evaluate(new FakeTree(), Composite);
        Assert.Equal(Is(DevnodeCondition.Missing, WatchedDevnode.Device), report.Hid);
        Assert.Equal(Is(DevnodeCondition.Missing, WatchedDevnode.Device), report.Audio);
    }

    [Fact]
    public void ACompositeWhoseUsbDeviceIsDisabledIsDisabledInBothItsPresentations()
    {
        var tree = new FakeTree();
        tree.Located[Composite.Device] = Problem(10, 22);
        PadReport report = PadEvaluation.Evaluate(tree, Composite);
        Assert.Equal(Is(DevnodeCondition.Disabled, WatchedDevnode.Device, 22), report.Hid);
        Assert.Equal(Is(DevnodeCondition.Disabled, WatchedDevnode.Device, 22), report.Audio);
    }

    [Fact]
    public void ACompositeWithoutAnAudioFunctionPresentsNone()
    {
        PadReport report = PadEvaluation.Evaluate(WholeComposite(out _, out _, out _), SilentComposite);
        Assert.Equal(Presents.Hid, report.Presents);
        Assert.Equal(default, report.Audio);
    }
}
