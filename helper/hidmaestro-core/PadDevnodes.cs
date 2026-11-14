// SPDX-License-Identifier: LGPL-3.0-or-later
namespace Satellite.HmHelper.Core;

// One devnode as CM_Locate_DevNode (normal) and CM_Get_DevNode_Status read it.
// Handle is the DEVINST a child walk starts from.
public readonly record struct DevnodeState(bool Located, uint Status, uint Problem, uint Handle);

public interface IDevnodeTree
{
    DevnodeState Locate(string instanceId);

    DevnodeState FirstChild(DevnodeState parent);

    DevnodeState ChildOfClass(DevnodeState parent, Guid setupClass);
}

public static class DevnodeStatus
{
    public const uint Started = 0x00000008;        // DN_STARTED
    public const uint HasProblem = 0x00000400;     // DN_HAS_PROBLEM
    public const uint PrivateProblem = 0x00008000; // DN_PRIVATE_PROBLEM
    public const uint WillBeRemoved = 0x00040000;  // DN_WILL_BE_REMOVED
    public const uint ProblemDisabled = 22;        // CM_PROB_DISABLED, Device Manager's disable

    public static DevnodeCondition Classify(DevnodeState node)
    {
        if (!node.Located) return DevnodeCondition.Missing;
        bool problem = (node.Status & (HasProblem | PrivateProblem)) != 0;
        if (problem) return node.Problem == ProblemDisabled ? DevnodeCondition.Disabled : DevnodeCondition.Failed;
        if ((node.Status & WillBeRemoved) != 0) return DevnodeCondition.Leaving;
        return (node.Status & Started) != 0 ? DevnodeCondition.Present : DevnodeCondition.Starting;
    }
}

// The devnodes a pad's controller owns, which the HIDMaestro SDK names from
// the identity's token and does not hand out (v2.1.0 DeviceNodeCreator.cs:130,
// PresentationPlan.cs:160-171, DeviceOrchestrator.cs:1146-1151,
// DeviceIdentity.cs:94, 106, UsbDescriptorSet.cs:146-151). Internal formats,
// held to the SDK by check-sdk; a HID child or a composite's functions are
// found by walking from the device.
public sealed record PadDevnodes(string Device, string? Companion, bool Usbip);

public static class DevnodeIds
{
    public const string PlainEnumerator = "HIDClass";
    public const string CompanionEnumerator = "HIDMAESTRO";
    private const string TokenPrefix = "HM_";
    private const int TokenDigits = 16;
    private const int SerialDigits = 12;

    // GUID_DEVCLASS_HIDCLASS: the setup class of a composite's HID function.
    public static readonly Guid HidClass = new("745a17a0-74d3-11d0-b6fe-00a0c90f57da");

    public static PadDevnodes For(ushort vendorId, ushort productId, bool companion, bool usbip, string token)
    {
        if (usbip) return new($@"USB\VID_{vendorId:X4}&PID_{productId:X4}\{UsbSerial(token)}", null, true);
        if (companion)
            return new($@"ROOT\{CompanionParentEnumerator(vendorId, productId)}\{token}",
                       $@"SWD\{CompanionEnumerator}\{token}", false);
        return new($@"ROOT\{PlainEnumerator}\{token}", null, false);
    }

    public static string CompanionParentEnumerator(ushort vendorId, ushort productId) =>
        $"VID_{vendorId:X4}&PID_{productId:X4}&IG_00";

    // The serial a composite with none of its own serves: the token's hash,
    // its low 48 bits, after "HM".
    public static string UsbSerial(string token)
    {
        bool keyed = token.Length == TokenPrefix.Length + TokenDigits &&
                     token.StartsWith(TokenPrefix, StringComparison.Ordinal);
        if (!keyed) throw new ArgumentException($"'{token}' is not a keyed identity token", nameof(token));
        return "HM" + token[^SerialDigits..];
    }
}

public static class PadEvaluation
{
    // Every devnode the pad needs, as far as a walk can reach: a disabled one
    // first, since disabling a devnode takes its children out of the tree, then
    // the first that is not present, else the pad is present.
    public static PadCondition Evaluate(IDevnodeTree tree, PadDevnodes pad)
    {
        DevnodeState device = tree.Locate(pad.Device);
        IEnumerable<(WatchedDevnode Role, DevnodeState Node)> found = new[] { (WatchedDevnode.Device, device) };
        if (DevnodeStatus.Classify(device) == DevnodeCondition.Present)
            found = found.Concat(pad.Usbip ? CompositeHid(tree, device) : ChildHid(tree, device));
        if (pad.Companion != null) found = found.Append((WatchedDevnode.Companion, tree.Locate(pad.Companion)));
        return Worst(found.ToArray());
    }

    private static IEnumerable<(WatchedDevnode, DevnodeState)> ChildHid(IDevnodeTree tree, DevnodeState parent)
    {
        yield return (WatchedDevnode.HidCollection, tree.FirstChild(parent));
    }

    private static IEnumerable<(WatchedDevnode, DevnodeState)> CompositeHid(IDevnodeTree tree, DevnodeState usb)
    {
        DevnodeState function = tree.ChildOfClass(usb, DevnodeIds.HidClass);
        yield return (WatchedDevnode.HidFunction, function);
        if (DevnodeStatus.Classify(function) == DevnodeCondition.Present)
            yield return (WatchedDevnode.HidCollection, tree.FirstChild(function));
    }

    private static PadCondition Worst(IReadOnlyList<(WatchedDevnode Role, DevnodeState Node)> found)
    {
        Finding[] findings = found.Select(f => new Finding(f.Role, f.Node, DevnodeStatus.Classify(f.Node))).ToArray();
        Finding? worst = findings.FirstOrDefault(f => f.Condition == DevnodeCondition.Disabled)
                         ?? findings.FirstOrDefault(f => f.Condition != DevnodeCondition.Present);
        return worst?.Reported() ?? PadCondition.Present;
    }

    private sealed record Finding(WatchedDevnode Role, DevnodeState Node, DevnodeCondition Condition)
    {
        public PadCondition Reported() => new(Condition, Role, Condition == DevnodeCondition.Missing ? 0 : Node.Problem);
    }
}
