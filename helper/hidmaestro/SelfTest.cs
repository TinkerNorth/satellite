// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Runtime.InteropServices;
using HIDMaestro;
using Satellite.HmHelper.Core;

namespace Satellite.HmHelper;

internal static class SelfTest
{
    private const int WatchMs = 500;
    private const uint MinClockFrames = 20;
    private const string KeyPrefix = "satellite:selftest:";
    private static readonly TimeSpan HidChildBudget = TimeSpan.FromSeconds(10);
    private static readonly byte[] SonyIdentityCalibration =
        Convert.FromHexString("000000000000401FC0E0401FC0E0401FC0E0F401F401002000E0002000E0002000E0");
    private static readonly byte[] SonyPairingMacPrefix = { 0x02, 0x48, 0x4D, 0x00, 0x00 };

    public static int Run(string? logPath)
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
        bool xbox = profileId.StartsWith("xbox-", StringComparison.Ordinal);
        string? unknown = SectionNames.FirstUnknown(SectionNames.Opened(transport, xbox), controller.SectionNames);
        check(unknown == null, unknown == null
            ? $"{profileId}: every section the helper opens is one the SDK lists for transport index {transport}"
            : $"{profileId}: the SDK lists no {unknown}");
        CheckVersionSection(controller, profileId, xbox, say, check);
        return controller;
    }

    // What Satellite's plug checks before its first frame
    // (src/core/hidmaestro_version.h), held to the SDK's own readings.
    private static void CheckVersionSection(HMController controller, string profileId, bool xbox,
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
        if (xbox)
            check(v.CompanionConfigError == 0 && v.CompanionProtocolMin <= 4 && 4 <= v.CompanionProtocol,
                  $"{profileId}: its XUSB companion serves protocols {v.CompanionProtocolMin} to {v.CompanionProtocol}, 4 among them, with error {v.CompanionConfigError}");
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
