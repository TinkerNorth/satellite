// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using HIDMaestro;
using Satellite.HmHelper.Core;

namespace Satellite.HmHelper;

internal static class SelfTest
{
    private const int WatchMs = 500;
    private const uint MinClockFrames = 20;
    private const string KeyPrefix = "satellite:selftest:";
    private const int AwaitPollMs = 50;
    private static readonly TimeSpan WatchBudget = TimeSpan.FromSeconds(15);
    // A composite whose USB device was disabled may drop its USB/IP connection,
    // which usbip-win2 attaches again 30 s later.
    private static readonly TimeSpan ReattachBudget = TimeSpan.FromSeconds(45);
    private static readonly TimeSpan HidChildBudget = TimeSpan.FromSeconds(10);
    // How long the hold ahead of an unplug's removal lasts if nothing ends it:
    // an unplug that waited for its removal answers only after it.
    private static readonly TimeSpan HoldBudget = TimeSpan.FromSeconds(20);
    private static readonly byte[] SonyIdentityCalibration =
        Convert.FromHexString("000000000000401FC0E0401FC0E0401FC0E0F401F401002000E0002000E0002000E0");
    private static readonly byte[] SonyPairingMacPrefix = { 0x02, 0x48, 0x4D, 0x00, 0x00 };

    public static int Run(string? logPath, bool composite)
    {
        var lines = new List<string>();
        int failures = 0;
        void Say(string text)
        {
            Console.WriteLine(text);
            lines.Add(text);
        }
        void Check(bool ok, string what)
        {
            Say((ok ? "PASS " : "FAIL ") + what);
            if (!ok) failures++;
        }

        Say("Close every program that holds a game controller open before this runs, Steam with a " +
            "PlayStation pad among them: one that refuses a removal makes each removal wait about 5.4 s " +
            "and fails the removal checks here (the SDK's DeviceManager.QueryRemoveSubtree).");
        try
        {
            using var ctx = new HMContext();
            ProfileSet profiles = ProfileSet.Load(ctx, ProfileCommands.InstalledDirectory);
            foreach (string issue in profiles.Issues) Say("issue: " + issue);
            Check(profiles.Issues.Count == 0, $"the profile copies in {profiles.Directory} load without issues");

            ctx.InstallDriver();
            Say($"driver: {DriverCommands.Describe(ctx.LastDriverInstall)}");
            HMContext.RemoveAllVirtualControllers(preserveInstall: true);

            uint frames = WatchIdle(ctx, "dualsense", 0, Say, Check);
            Check(frames > 0, $"the catalog dualsense profile's SDK idle repeat writes the input section by itself: {frames} frames in {WatchMs} ms (the hazard the copies avoid)");

            frames = WatchIdle(ctx, SatelliteProfiles.CopyId("dualsense"), 1, Say, Check);
            Check(frames == 0, $"satellite-dualsense: nothing but Satellite writes its input section, {frames} frames in {WatchMs} ms");

            frames = WatchIdle(ctx, SatelliteProfiles.CopyId("dualshock-4-v2"), 2, Say, Check);
            Check(frames == 0, $"satellite-dualshock-4-v2: nothing but Satellite writes its input section, {frames} frames in {WatchMs} ms");

            frames = WatchIdle(ctx, "xbox-360-wired", 3, Say, Check);
            Check(frames == 0, $"xbox-360-wired: nothing but Satellite writes its input section, {frames} frames in {WatchMs} ms");

            SwitchPro(ctx, 4, Say, Check);

            FeatureAnswers(ctx, SatelliteProfiles.CopyId("dualsense"), 5, calibrationReport: 0x05, pairingReport: 0x09, Say, Check);
            FeatureAnswers(ctx, SatelliteProfiles.CopyId("dualshock-4-v2"), 6, calibrationReport: 0x02, pairingReport: 0x12, Say, Check);

            WatchedDevices(SatelliteProfiles.CopyId("dualsense"), 7, ctx.GetProfile(SatelliteProfiles.CopyId("dualsense")), Say, Check);
            WatchedDevices("xbox-360-wired", 8, null, Say, Check);
            if (composite)
            {
                if (!HMContext.IsUsbipBackendAvailable) HMContext.InstallUsbipBackend();
                WatchedDevices(SatelliteProfiles.CopyId("dualsense-composite"), 9,
                               ctx.GetProfile(SatelliteProfiles.CopyId("dualsense-composite")), Say, Check);
            }
            else
            {
                Say("the composite's device watch is not checked; run selftest --composite to check it (installs HIDMaestro's USB/IP transport)");
            }
        }
        catch (Exception ex)
        {
            Say("FAIL " + ex);
            failures++;
        }

        Say(failures == 0 ? "selftest: all passed" : $"selftest: {failures} failed");
        if (logPath != null) File.WriteAllLines(logPath, lines);
        return failures == 0 ? 0 : 1;
    }

    private static HMController Create(HMContext ctx, string profileId, int slot, Action<string> say, Action<bool, string> check)
    {
        HMProfile profile = ctx.GetProfile(profileId) ?? throw new InvalidOperationException($"no profile '{profileId}'");
        HMController controller = ctx.CreateControllerAt(slot, profile, KeyPrefix + profileId);
        int transport = controller.TransportIndex;
        say($"{profileId}: slot {slot}, transport index {transport}, driver {controller.DriverVersion?.ToString() ?? "unknown"}, {controller.SectionNames.Count} sections");
        bool companion = XusbCompanion.Presents(profile.XInput?.Via, profile.RequiresUsbipBackend);
        string? unknown = SectionNames.FirstUnknown(SectionNames.Opened(transport, companion), controller.SectionNames);
        check(unknown == null, unknown == null
            ? $"{profileId}: every section the helper opens is one the SDK lists for transport index {transport}"
            : $"{profileId}: the SDK lists no {unknown}");
        CheckVersionSection(controller, profileId, companion, say, check);
        return controller;
    }

    // What Satellite's plug checks before its first frame
    // (src/core/hidmaestro_version.h), held to the SDK's own readings.
    private static void CheckVersionSection(HMController controller, string profileId, bool companion,
                                            Action<string> say, Action<bool, string> check)
    {
        VersionSection v = ReadVersionSection(controller.TransportIndex);
        Version sdk = typeof(HMContext).Assembly.GetName().Version ?? new Version(0, 0, 0);
        Version? driver = controller.DriverVersion;
        ulong tag = IdentityTags.OfKey(controller.IdentityKey);
        say($"{profileId}: version section layout {v.LayoutVersion}, SDK {v.SdkVersion} protocols {v.SdkProtocolMin} to {v.SdkProtocolMax}, " +
            $"driver {v.DriverVersion} protocols {v.DriverProtocolMin} to {v.DriverProtocol} error {v.DriverConfigError}, " +
            $"companion protocols {v.CompanionProtocolMin} to {v.CompanionProtocol} error {v.CompanionConfigError}, tag {v.IdentityTag:X16}");
        check(v.LayoutVersion == 1 && v.SdkProtocolMin == 4 && v.SdkProtocolMax == 4 && SameRelease(v.SdkVersion, sdk),
              $"{profileId}: the version section's SDK half is layout 1, protocols 4 to 4, and this SDK's {sdk}");
        check(v.IdentityTag == tag,
              $"{profileId}: its identity tag {v.IdentityTag:X16} is the one Satellite derives from the key '{controller.IdentityKey}', {tag:X16}");
        check(v.DriverConfigError == 0 && v.DriverProtocolMin <= 4 && 4 <= v.DriverProtocol,
              $"{profileId}: its driver serves protocols {v.DriverProtocolMin} to {v.DriverProtocol}, 4 among them, with error {v.DriverConfigError}");
        check(driver != null && SameRelease(v.DriverVersion, driver),
              $"{profileId}: its driver version {v.DriverVersion} is the one the SDK reads, {driver?.ToString() ?? "none"}");
        if (!companion) return;
        // The companion writes its pair only at its add, and the SDK restarts
        // one that is already running without waiting for it, so an unwritten
        // pair passes, as it does at Satellite's plug.
        if (v.CompanionProtocol == 0)
            say($"{profileId}: its XUSB companion has not written its pair; a companion the SDK restarted writes none");
        check(v.CompanionConfigError == 0 && (v.CompanionProtocol == 0 || (v.CompanionProtocolMin <= 4 && 4 <= v.CompanionProtocol)),
              $"{profileId}: its XUSB companion has error {v.CompanionConfigError} and serves protocols {v.CompanionProtocolMin} to {v.CompanionProtocol}, 4 among them where written");
    }

    private static bool SameRelease(Version a, Version b) => a.Major == b.Major && a.Minor == b.Minor && a.Build == b.Build;

    // Opened FILE_MAP_READ alone, the access satellite.exe's duplicate carries.
    private static VersionSection ReadVersionSection(int transportIndex)
    {
        string name = SectionNames.Version(transportIndex);
        nint mapping = NativeMethods.OpenFileMappingW(NativeMethods.FILE_MAP_READ, false, name);
        if (mapping == 0)
            throw new InvalidOperationException($"could not open {name} for reading (error {Marshal.GetLastWin32Error()})");
        try
        {
            nint view = NativeMethods.MapViewOfFile(mapping, NativeMethods.FILE_MAP_READ, 0, 0, (nuint)VersionSection.Size);
            if (view == 0)
                throw new InvalidOperationException($"could not map {VersionSection.Size} bytes of {name} (error {Marshal.GetLastWin32Error()})");
            try
            {
                var bytes = new byte[VersionSection.Size];
                Marshal.Copy(view, bytes, 0, bytes.Length);
                return VersionSection.Parse(bytes);
            }
            finally
            {
                NativeMethods.UnmapViewOfFile(view);
            }
        }
        finally
        {
            NativeMethods.CloseHandle(mapping);
        }
    }

    private static uint WatchIdle(HMContext ctx, string profileId, int slot, Action<string> say, Action<bool, string> check)
    {
        using HMController controller = Create(ctx, profileId, slot, say, check);
        using var input = new InputSectionView(controller.TransportIndex);
        uint before = input.SeqNo;
        Thread.Sleep(WatchMs);
        return (input.SeqNo - before) / 2;
    }

    private static void SwitchPro(HMContext ctx, int slot, Action<string> say, Action<bool, string> check)
    {
        string profileId = SatelliteProfiles.CopyId("switch-pro");
        HMProfile profile = ctx.GetProfile(profileId) ?? throw new InvalidOperationException($"no profile '{profileId}'");
        IReadOnlyList<string> before = HidDevices.Paths(profile.VendorId, profile.ProductId);
        using HMController controller = Create(ctx, profileId, slot, say, check);
        check(controller.Profile.BuildsItsOwnReports, $"{profileId} builds its own reports, so the helper relays Satellite's state block to the SDK");
        using var input = new InputSectionView(controller.TransportIndex);

        uint frames = FramesWithin(input, WatchMs);
        check(frames >= MinClockFrames, $"{profileId}: the simple report streams on the clock before any host speaks the protocol, {frames} frames in {WatchMs} ms, report 0x{input.ExtendedReportId:X2}");

        try
        {
            controller.SubmitRawReport(new byte[47]);
            check(false, "a 47-byte state block is refused");
        }
        catch (ArgumentException ex)
        {
            check(ex.Message.Contains("48"), $"a 47-byte state block is refused, naming the size: {ex.Message}");
        }

        var block = new byte[48];
        block[2] = 0x04;
        controller.SubmitRawReport(block);
        frames = FramesWithin(input, WatchMs);
        check(frames >= MinClockFrames, $"a 48-byte state block is accepted and the SDK keeps writing the input section itself: {frames} frames in {WatchMs} ms");

        string? path = HidDevices.WaitForNewPath(profile.VendorId, profile.ProductId, before, HidChildBudget);
        check(path != null, $"{profileId}: Windows exposes a new {profile.VendorId:X4}:{profile.ProductId:X4} HID device for it");
        if (path == null) return;

        check(HidDevices.WriteOutput(path, SwitchSubcommand(1, 0x03, 0x30)), $"{profileId}: a host can select full mode (0x30) through the HID device");
        frames = FramesWithin(input, WatchMs);
        check(frames >= MinClockFrames && input.ExtendedReportId == 0x30, $"{profileId}: full mode streams report 0x30 on the clock, {frames} frames in {WatchMs} ms, report 0x{input.ExtendedReportId:X2}");

        check(HidDevices.WriteOutput(path, SwitchSubcommand(2, 0x03, 0x3F)), $"{profileId}: a host can select simple mode (0x3F) through the HID device");
        frames = FramesWithin(input, WatchMs);
        check(frames >= MinClockFrames && input.ExtendedReportId == 0x3F, $"{profileId}: simple mode still streams report 0x3F on the clock, {frames} frames in {WatchMs} ms, report 0x{input.ExtendedReportId:X2}, so an SDL2-era host never sees three silent seconds");
    }

    private static byte[] SwitchSubcommand(byte packet, byte id, byte argument)
    {
        var report = new byte[49];
        report[0] = 0x01;
        report[1] = packet;
        report[2] = 0x00;
        report[3] = 0x01;
        report[4] = 0x40;
        report[5] = 0x40;
        report[6] = 0x00;
        report[7] = 0x01;
        report[8] = 0x40;
        report[9] = 0x40;
        report[10] = id;
        report[11] = argument;
        return report;
    }

    private static uint FramesWithin(InputSectionView input, int ms)
    {
        uint before = input.SeqNo;
        Thread.Sleep(ms);
        return (input.SeqNo - before) / 2;
    }

    private static void FeatureAnswers(HMContext ctx, string profileId, int slot, byte calibrationReport, byte pairingReport,
                                       Action<string> say, Action<bool, string> check)
    {
        HMProfile profile = ctx.GetProfile(profileId) ?? throw new InvalidOperationException($"no profile '{profileId}'");
        IReadOnlyList<string> before = HidDevices.Paths(profile.VendorId, profile.ProductId);
        using HMController controller = Create(ctx, profileId, slot, say, check);
        string? path = HidDevices.WaitForNewPath(profile.VendorId, profile.ProductId, before, HidChildBudget);
        check(path != null, $"{profileId}: Windows exposes a new {profile.VendorId:X4}:{profile.ProductId:X4} HID device for it");
        if (path == null) return;

        byte[]? calibration = HidDevices.GetFeature(path, calibrationReport, 41);
        string hex = calibration == null ? "no answer" : Convert.ToHexString(calibration, 0, Math.Min(calibration.Length, 41));
        check(calibration != null && calibration.Length >= 41 && calibration[0] == calibrationReport &&
              calibration.AsSpan(1, SonyIdentityCalibration.Length).SequenceEqual(SonyIdentityCalibration),
              $"{profileId}: feature 0x{calibrationReport:X2} is the Sony model's identity calibration ({hex})");

        byte[]? pairing = HidDevices.GetFeature(path, pairingReport, 20);
        string mac = pairing == null ? "no answer" : Convert.ToHexString(pairing, 0, Math.Min(pairing.Length, 7));
        check(pairing != null && pairing.Length >= 7 && pairing[0] == pairingReport &&
              pairing.AsSpan(1, SonyPairingMacPrefix.Length).SequenceEqual(SonyPairingMacPrefix) &&
              pairing[6] == controller.TransportIndex,
              $"{profileId}: feature 0x{pairingReport:X2} carries the pairing MAC ending in the transport index ({mac})");
    }

    // The device watch on a real controller, through the session satellite.exe
    // talks to: the hello on the watch's protocol, an unplug answered while its
    // device is still in Windows, the plug that hands over the block, the
    // devnodes the watch derived, each presentation disabled and enabled again
    // through the Configuration Manager on its own, and the device removed. A
    // composite's USB device is disabled too, and what the USB/IP connection
    // does then is reported.
    private static void WatchedDevices(string profileId, int slot, HMProfile? sony, Action<string> say, Action<bool, string> check)
    {
        IReadOnlyList<string> hidBefore = sony == null ? Array.Empty<string>() : HidDevices.Paths(sony.VendorId, sony.ProductId);
        using var session = new Session(Environment.ProcessId);
        string hello = session.Handle(@"{""op"":""hello"",""protocol"":2}", out _);
        check(hello.Contains(@"""protocol"":2"), $"{profileId}: the session settles on the device watch's protocol");
        var tree = new WindowsDevnodeTree();
        UnplugAnswersFirst(session, profileId, slot, tree, check);
        string reply = session.Handle($@"{{""op"":""plug"",""serial"":{slot},""profile"":""{profileId}""}}", out _);
        ulong handle = WatchHandleOf(reply);
        PadDevnodes? devnodes = session.WatchedDevnodes((uint)slot);
        check(handle != 0 && devnodes != null, $"{profileId}: the plug hands over a watch block ({Shorten(reply)})");
        if (handle == 0 || devnodes == null) return;
        say($"{profileId}: the watch looks for {devnodes.Device}{(devnodes.Companion == null ? "" : " and " + devnodes.Companion)}" +
            (devnodes.Audio ? ", with an audio function" : ""));
        using var block = new WatchBlockView(handle);

        DeviceWatchBlock.Reading whole = block.Await(r => r.Report is { Whole: true }, WatchBudget);
        check(whole.Report is { Whole: true }, $"{profileId}: the watch finds every presentation present, {Describe(whole)}");
        DeviceWatchBlock.Reading before = block.Read();
        Thread.Sleep(2 * DeviceWatchBlock.BeatMs);
        DeviceWatchBlock.Reading after = block.Read();
        check(after.Heartbeat != before.Heartbeat, $"{profileId}: the heartbeat moves");
        check(after.Seq != before.Seq, $"{profileId}: the watcher looks again, so the sequence moves with nothing changed");
        if (whole.Report is not { Whole: true }) return;

        using InputProbe? input = sony == null ? null : InputProbe.Open(TransportIndexOf(reply), sony, hidBefore);
        input?.Check(profileId, "before anything is disabled", check);

        DevnodeState device = tree.Locate(devnodes.Device);
        DevnodeState hidParent = devnodes.Usbip ? tree.ChildOfClass(device, DevnodeIds.HidClass) : device;
        say($"{profileId}: its HID collection is {WindowsDevnodeTree.InstanceIdOf(tree.FirstChild(hidParent)) ?? "not found"}");
        DisableAndEnable(profileId, block, tree.FirstChild(hidParent), Presents.Hid, WatchedDevnode.HidCollection, input, say, check);
        if (devnodes.Usbip)
            DisableAndEnable(profileId, block, tree.ChildOfClass(tree.Locate(devnodes.Device), DevnodeIds.HidClass),
                             Presents.Hid, WatchedDevnode.HidFunction, input, say, check);
        if (devnodes.Audio)
            DisableAndEnable(profileId, block, tree.ChildOfClass(tree.Locate(devnodes.Device), DevnodeIds.MediaClass),
                             Presents.Audio, WatchedDevnode.AudioFunction, input, say, check);
        if (devnodes.Companion != null)
            DisableAndEnable(profileId, block, tree.Locate(devnodes.Companion), Presents.Xinput, WatchedDevnode.Companion,
                             input, say, check);
        DisableAndEnable(profileId, block, tree.Locate(devnodes.Device), Presents.Hid, WatchedDevnode.Device, input, say, check);

        DevnodeState parent = tree.Locate(devnodes.Device);
        uint removed = CfgMgr.CM_Query_And_Remove_SubTreeW(parent.Handle, out int veto, null, 0, CfgMgr.CM_REMOVE_NO_RESTART);
        check(removed == CfgMgr.CR_SUCCESS, $"{profileId}: CM_Query_And_Remove_SubTree removes {devnodes.Device} (CONFIGRET {removed}, veto {veto})");
        DeviceWatchBlock.Reading gone = block.Await(r => r.Report?.Hid.Condition is DevnodeCondition.Missing or DevnodeCondition.Failed,
                                                    WatchBudget);
        bool goneOrHeld = gone.Report?.Hid is { Devnode: WatchedDevnode.Device, Condition: DevnodeCondition.Missing or DevnodeCondition.Failed };
        check(goneOrHeld, $"{profileId}: once it is removed the watch finds its device gone, or failed as a USB device held for " +
                          $"eject is (Code 47): {Describe(gone)}");
    }

    // A controller plugged and unplugged while its devices are all in Windows,
    // its removal queued behind a hold of the selftest's own: the removal cannot
    // begin before the hold ends, so an answer that came while the hold still
    // ran did not wait for the removal. Ordering, not timing: a composite's
    // removal can take a few milliseconds (INTERNALS.md). An unplug that removed
    // the controller before answering would answer only once the hold gave out.
    // The device then leaves on its own.
    private static void UnplugAnswersFirst(Session session, string profileId, int slot, WindowsDevnodeTree tree,
                                           Action<bool, string> check)
    {
        string reply = session.Handle($@"{{""op"":""plug"",""serial"":{slot},""profile"":""{profileId}""}}", out _);
        PadDevnodes? devnodes = session.WatchedDevnodes((uint)slot);
        ulong handle = WatchHandleOf(reply);
        check(handle != 0 && devnodes != null, $"{profileId}: plugged for the unplug check ({Shorten(reply)})");
        if (handle == 0 || devnodes == null) return;
        using var block = new WatchBlockView(handle);
        DeviceWatchBlock.Reading whole = block.Await(r => r.Report is { Whole: true }, WatchBudget);
        check(whole.Report is { Whole: true }, $"{profileId}: every presentation reads present before the unplug, {Describe(whole)}");
        using var release = new ManualResetEventSlim(false);
        int holdEnded = 0;
        session.HoldRemovals((uint)slot, () =>
        {
            release.Wait(HoldBudget);
            Volatile.Write(ref holdEnded, 1);
        });
        var clock = Stopwatch.StartNew();
        string unplugged = session.Handle($@"{{""op"":""unplug"",""serial"":{slot}}}", out _);
        long answeredMs = clock.ElapsedMilliseconds;
        bool answeredFirst = Volatile.Read(ref holdEnded) == 0;
        bool stillThere = tree.Locate(devnodes.Device).Located;
        check(unplugged.Contains(@"""ok"":true") && answeredFirst && stillThere,
              $"{profileId}: the session answers its unplug in {answeredMs} ms while its removal waits behind the selftest's hold, " +
              "the device still in Windows, so the answer never waits for the removal");
        release.Set();
        DevnodeCondition after = AwaitGone(tree, devnodes.Device, ReattachBudget);
        check(after is DevnodeCondition.Missing or DevnodeCondition.Failed,
              $"{profileId}: the device then leaves Windows, or stays held for eject (Code 47): {after}, " +
              $"{clock.Elapsed.TotalSeconds:F1} s after the unplug");
    }

    // Until the devnode is out of the tree, or in it with a problem as a USB
    // device held for eject is.
    private static DevnodeCondition AwaitGone(WindowsDevnodeTree tree, string instanceId, TimeSpan budget)
    {
        var clock = Stopwatch.StartNew();
        DevnodeCondition now = DevnodeStatus.Classify(tree.Locate(instanceId));
        while (now is not (DevnodeCondition.Missing or DevnodeCondition.Failed) && clock.Elapsed < budget)
        {
            Thread.Sleep(AwaitPollMs);
            now = DevnodeStatus.Classify(tree.Locate(instanceId));
        }
        return now;
    }

    // Disables one devnode and checks that its presentation, and only that
    // one, reads disabled; then enables it and times the presentation's return.
    private static void DisableAndEnable(string profileId, WatchBlockView block, DevnodeState node, Presents part,
                                         WatchedDevnode role, InputProbe? input, Action<string> say, Action<bool, string> check)
    {
        string named = $"{profileId}: its {role}";
        uint disabled = node.Located ? CfgMgr.CM_Disable_DevNode(node.Handle, CfgMgr.CM_DISABLE_UI_NOT_OK) : uint.MaxValue;
        check(disabled == CfgMgr.CR_SUCCESS, $"{named} is disabled through CM_Disable_DevNode (CONFIGRET {disabled})");
        if (disabled != CfgMgr.CR_SUCCESS) return;
        var expected = new PresentationCondition(DevnodeCondition.Disabled, role, DevnodeStatus.ProblemDisabled);
        DeviceWatchBlock.Reading off = block.Await(r => r.Report?.Of(part) == expected, WatchBudget);
        check(off.Report?.Of(part) == expected, $"{named} disabled reads disabled with Code 22, {Describe(off)}");
        foreach (Presents other in new[] { Presents.Hid, Presents.Xinput })
        {
            bool beside = other != part && off.Report is { } report && report.Presents.HasFlag(other);
            if (beside)
                check(off.Report!.Value.Present(other), $"{named} disabled, its {other} presentation still reads present, {Describe(off)}");
        }
        uint enabled = CfgMgr.CM_Enable_DevNode(node.Handle, 0);
        check(enabled == CfgMgr.CR_SUCCESS, $"{named} is enabled again through CM_Enable_DevNode (CONFIGRET {enabled})");
        var started = Stopwatch.StartNew();
        DeviceWatchBlock.Reading back = block.Await(r => r.Report is { Whole: true }, ReattachBudget);
        check(back.Report is { Whole: true },
              $"{named} enabled again, the watch finds every presentation present after {started.Elapsed.TotalSeconds:F1} s, {Describe(back)}");
        input?.Check(profileId, $"after its {role} was enabled again", check);
    }

    private static int TransportIndexOf(string reply)
    {
        using JsonDocument doc = JsonDocument.Parse(reply);
        return doc.RootElement.TryGetProperty("transportIndex", out JsonElement transport) && transport.TryGetInt32(out int index)
            ? index
            : throw new InvalidOperationException($"the plug reply names no transport index: {Shorten(reply)}");
    }

    private sealed class InputProbe : IDisposable
    {
        private const int SonyPayloadBytes = 63;
        private const byte SonyStickCentre = 0x80;
        private const byte SonyHatNeutral = 0x08;
        private static readonly TimeSpan ReportBudget = TimeSpan.FromSeconds(1);
        private static readonly TimeSpan ResumeBudget = TimeSpan.FromSeconds(10);

        private readonly nint _mapping;
        private readonly nint _view;
        private readonly EventWaitHandle _doorbell;
        private readonly HMProfile _profile;
        private readonly IReadOnlyList<string> _before;
        private byte _mark = 0x10;
        private byte _seq;

        private InputProbe(nint mapping, nint view, EventWaitHandle doorbell, HMProfile profile, IReadOnlyList<string> before)
        {
            _mapping = mapping;
            _view = view;
            _doorbell = doorbell;
            _profile = profile;
            _before = before;
        }

        public static InputProbe Open(int transportIndex, HMProfile profile, IReadOnlyList<string> before)
        {
            string name = SectionNames.Input(transportIndex);
            uint access = NativeMethods.FILE_MAP_READ | NativeMethods.FILE_MAP_WRITE;
            nint mapping = NativeMethods.OpenFileMappingW(access, false, name);
            if (mapping == 0)
                throw new InvalidOperationException($"could not open {name} for writing (error {Marshal.GetLastWin32Error()})");
            nint view = NativeMethods.MapViewOfFile(mapping, access, 0, 0, (nuint)FrameMailbox.SectionSize);
            if (view == 0)
            {
                int error = Marshal.GetLastWin32Error();
                NativeMethods.CloseHandle(mapping);
                throw new InvalidOperationException($"could not map {name} for writing (error {error})");
            }
            return new InputProbe(mapping, view, EventWaitHandle.OpenExisting(SectionNames.InputEvent(transportIndex)), profile, before);
        }

        public void Check(string profileId, string when, Action<bool, string> check)
        {
            byte mark = ++_mark;
            bool delivered = Delivers(mark, out string said);
            check(delivered, $"{profileId}: a frame written to its input section {when} comes out of its HID collection as an input report: {said}");
        }

        private bool Delivers(byte mark, out string said)
        {
            var clock = Stopwatch.StartNew();
            said = $"no {_profile.VendorId:X4}:{_profile.ProductId:X4} HID device of its own is present";
            while (clock.Elapsed < ResumeBudget)
            {
                string? path = HidDevices.Paths(_profile.VendorId, _profile.ProductId)
                    .FirstOrDefault(p => !_before.Contains(p, StringComparer.OrdinalIgnoreCase));
                if (path == null)
                {
                    Thread.Sleep(AwaitPollMs);
                    continue;
                }
                HidDevices.InputReader reader;
                try
                {
                    reader = new HidDevices.InputReader(path);
                }
                catch (InvalidOperationException ex)
                {
                    said = ex.Message;
                    Thread.Sleep(AwaitPollMs);
                    continue;
                }
                using (reader)
                {
                    WriteFrame(mark);
                    TimeSpan deadline = clock.Elapsed + ReportBudget;
                    while (clock.Elapsed < deadline)
                    {
                        byte[]? report;
                        try
                        {
                            report = reader.Read(ReportBudget);
                        }
                        catch (IOException ex)
                        {
                            said = ex.Message;
                            break;
                        }
                        if (report == null)
                        {
                            said = $"no input report within {ReportBudget.TotalSeconds:F0} s of the frame";
                            break;
                        }
                        if (report.Length > 1 && report[1] == mark)
                        {
                            said = $"report 0x{report[0]:X2} carries left stick X 0x{mark:X2}, {clock.Elapsed.TotalSeconds:F1} s in";
                            return true;
                        }
                        said = $"an input report carried left stick X 0x{(report.Length > 1 ? report[1] : 0):X2}, not 0x{mark:X2}";
                    }
                }
            }
            return false;
        }

        private unsafe void WriteFrame(byte mark)
        {
            var frame = new byte[SonyPayloadBytes];
            frame[0] = mark;
            frame[1] = SonyStickCentre;
            frame[2] = SonyStickCentre;
            frame[3] = SonyStickCentre;
            frame[6] = ++_seq;
            frame[7] = SonyHatNeutral;
            FrameMailbox.Write(new Span<byte>((void*)_view, FrameMailbox.SectionSize), frame);
            _doorbell.Set();
        }

        public void Dispose()
        {
            _doorbell.Dispose();
            NativeMethods.UnmapViewOfFile(_view);
            NativeMethods.CloseHandle(_mapping);
        }
    }

    private static string Describe(DeviceWatchBlock.Reading reading) =>
        reading.Report is PadReport r
            ? $"HID {Part(r.Hid)}" + (r.Presents.HasFlag(Presents.Xinput) ? $"; XInput {Part(r.Xinput)}" : "") +
              (r.Presents.HasFlag(Presents.Audio) ? $"; audio {Part(r.Audio)}" : "")
            : "mid-write";

    private static string Part(PresentationCondition p) => $"{p.Condition} {p.Devnode} problem {p.Problem}";

    private static ulong WatchHandleOf(string reply)
    {
        using JsonDocument doc = JsonDocument.Parse(reply);
        return doc.RootElement.TryGetProperty("watch", out JsonElement watch) && watch.TryGetUInt64(out ulong handle)
            ? handle
            : 0;
    }

    private static string Shorten(string reply) => reply.Length <= 120 ? reply : reply[..120] + "...";

    private sealed class WatchBlockView : IDisposable
    {
        private readonly nint _mapping;
        private readonly nint _view;

        public WatchBlockView(ulong handle)
        {
            _mapping = (nint)handle;
            _view = _mapping == 0 ? 0 : NativeMethods.MapViewOfFile(_mapping, NativeMethods.FILE_MAP_READ, 0, 0, (nuint)DeviceWatchBlock.BlockSize);
            if (_view == 0) throw new InvalidOperationException($"could not map the watch block read-only (error {Marshal.GetLastWin32Error()})");
        }

        public unsafe DeviceWatchBlock.Reading Read() =>
            DeviceWatchBlock.Read(new ReadOnlySpan<byte>((void*)_view, DeviceWatchBlock.BlockSize));

        public DeviceWatchBlock.Reading Await(Func<DeviceWatchBlock.Reading, bool> done, TimeSpan budget)
        {
            var clock = Stopwatch.StartNew();
            DeviceWatchBlock.Reading reading = Read();
            while (!done(reading) && clock.Elapsed < budget)
            {
                Thread.Sleep(AwaitPollMs);
                reading = Read();
            }
            return reading;
        }

        public void Dispose()
        {
            NativeMethods.UnmapViewOfFile(_view);
            NativeMethods.CloseHandle(_mapping);
        }
    }

    private sealed class InputSectionView : IDisposable
    {
        private readonly nint _mapping;
        private readonly nint _view;

        public InputSectionView(int transportIndex)
        {
            string name = SectionNames.Input(transportIndex);
            _mapping = NativeMethods.OpenFileMappingW(NativeMethods.FILE_MAP_READ, false, name);
            if (_mapping == 0)
                throw new InvalidOperationException($"could not open {name} (error {Marshal.GetLastWin32Error()})");
            _view = NativeMethods.MapViewOfFile(_mapping, NativeMethods.FILE_MAP_READ, 0, 0, (nuint)FrameMailbox.SectionSize);
            if (_view == 0)
            {
                NativeMethods.CloseHandle(_mapping);
                throw new InvalidOperationException($"could not map {name} (error {Marshal.GetLastWin32Error()})");
            }
        }

        public uint SeqNo => (uint)Marshal.ReadInt32(_view, FrameMailbox.SeqNoOffset);

        public byte ExtendedReportId => Marshal.ReadByte(_view, ExtendedDataOffset);

        private const int ExtendedDataOffset = 282;

        public void Dispose()
        {
            NativeMethods.UnmapViewOfFile(_view);
            NativeMethods.CloseHandle(_mapping);
        }
    }
}
