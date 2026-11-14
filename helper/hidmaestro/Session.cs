// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Text;
using System.Text.Json;
using HIDMaestro;
using Satellite.HmHelper.Core;

namespace Satellite.HmHelper;

// Pipe protocol (one JSON line per request, one per response).
//
//   {"op":"hello","protocol":1}
//     -> {"ok":true,"helper":"satellite-hm-helper","audio":true,"broker":B}
//
//   {"op":"plug","serial":N,"profile":"<catalog id>"}
//     -> {"ok":true,"index":N,"transportIndex":T,"input":H,"inputEvent":H,
//         "companionEvent":H,"output":H,"outputEvent":H,
//         "version":H,"identityTag":U,"usbip":B,"companion":B,"timeoutScale":F,
//         "speakerAudio":H,"speakerAudioEvent":H,"micAudio":H,"micAudioEvent":H,
//         "speakerChannels":N,"speakerRateHz":N,"micChannels":N,"micRateHz":N,
//         "hapticAudio":H,"hapticAudioEvent":H,"hapticChannels":N}
//
// Every H is a handle already duplicated into the satellite process, which
// owns it. `input` and `inputEvent` are where satellite's frames go: the
// driver's own input section for a profile satellite writes natively, or a
// frame mailbox of the same layout that this process relays to the SDK for a
// profile whose reports the SDK builds itself (hidmaestro_wire.h pins the
// layout; FrameMailbox mirrors it). `version` is the controller's version
// section, opened FILE_MAP_READ alone so satellite can read it and never
// write it; `identityTag` is the tag the SDK writes there for the
// controller's identity key, and `usbip` says the SDK's USB/IP device serves a
// composite persona, which writes no driver half. `companion` says the profile
// reaches XInput through an XUSB companion, which writes its own pair, and
// `timeoutScale` is HIDMAESTRO_TIMEOUT_SCALE as this process's SDK resolves it
// (SdkTimeoutScale), which scales Satellite's wait for the driver half as it
// scales the SDK's. It is left out when the SDK took a NaN, which JSON cannot
// carry; satellite.exe then waits unscaled.
// Satellite checks the section before its first frame
// (src/core/hidmaestro_version.h). The audio fields are
// present only when the profile materialized a USB-audio function, and the
// haptic ones only when that endpoint carries the HD-haptics lanes.
internal sealed class Session : IDisposable
{
    // Read once, as the SDK reads it at its TimeoutScale's type init.
    private static readonly double TimeoutScale =
        SdkTimeoutScale.Resolve(Environment.GetEnvironmentVariable(SdkTimeoutScale.Variable));

    private readonly int _parentPid;
    private readonly bool _broker;
    private readonly HMContext _ctx = new();
    private readonly Dictionary<uint, PluggedController> _plugged = new();
    private ProfileSet? _profiles;
    private bool _disposed;

    public Session(int parentPid, bool broker = false)
    {
        _parentPid = parentPid;
        _broker = broker;
    }

    public string Handle(string line, out bool quit)
    {
        quit = false;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(line);
            JsonElement root = doc.RootElement;
            string op = root.GetProperty("op").GetString() ?? "";
            switch (op)
            {
                case "hello":
                    return Ok(w =>
                    {
                        w.WriteString("helper", "satellite-hm-helper");
                        w.WriteBoolean("audio", true);
                        w.WriteBoolean("broker", _broker);
                    });
                case "plug":
                    return Plug(root.GetProperty("serial").GetUInt32(),
                                root.GetProperty("profile").GetString() ?? "");
                case "unplug":
                    return Unplug(root.GetProperty("serial").GetUInt32());
                case "shutdown":
                    quit = true;
                    return Ok(_ => { });
                default:
                    return Error($"unknown op '{op}'");
            }
        }
        catch (Exception ex)
        {
            return Error(ex.Message);
        }
    }

    private ProfileSet Profiles()
    {
        if (_profiles != null) return _profiles;
        HMContext.RemoveAllVirtualControllers(preserveInstall: true);
        _profiles = ProfileSet.Load(_ctx, ProfileCommands.InstalledDirectory);
        foreach (string issue in _profiles.Issues)
            Console.Error.WriteLine($"satellite-hm-helper: {issue}");
        return _profiles;
    }

    private string Plug(uint serial, string profileId)
    {
        ProfileSet profiles = Profiles();
        Unplug(serial);

        HMProfile? profile = profiles.Resolve(_ctx, profileId, out string? refusal);
        if (profile == null) return Error(refusal ?? $"unknown profile '{profileId}'");

        if (profile.RequiresUsbipBackend && !HMContext.IsUsbipBackendAvailable)
            HMContext.InstallUsbipBackend();

        HMController controller;
        try
        {
            controller = _ctx.CreateControllerAt(checked((int)serial), profile,
                                                 SatelliteProfiles.IdentityKey(serial, profileId));
        }
        catch (HMResourceInUseException ex)
        {
            return Error($"another program holds this controller's identity: {ex.Message}");
        }
        catch (HMDriverVersionException ex)
        {
            return Error($"the installed HIDMaestro driver {ex.InstalledVersion} cannot run this SDK's controller ({ex.Problem}): {ex.Message}");
        }

        var plugged = new PluggedController(controller);
        try
        {
            _ctx.FinalizeNames();
            int transport = controller.TransportIndex;
            bool companion = XusbCompanion.Presents(profile.XInput?.Via, profile.RequiresUsbipBackend);
            string? unknown = SectionNames.FirstUnknown(SectionNames.Opened(transport, companion), controller.SectionNames);
            if (unknown != null)
                throw new InvalidOperationException($"the SDK lists no {unknown} among the sections of transport index {transport}");

            ulong input;
            ulong inputEvent;
            if (profile.BuildsItsOwnReports)
            {
                plugged.Relay = FrameRelay.Start(controller, DuplicateForParent);
                input = plugged.Relay.SectionHandle;
                inputEvent = plugged.Relay.DoorbellHandle;
            }
            else
            {
                input = DuplicateMapping(SectionNames.Input(transport));
                inputEvent = DuplicateEvent(SectionNames.InputEvent(transport));
            }
            ulong companionEvent = companion ? DuplicateEvent(SectionNames.CompanionInputEvent(transport)) : 0;
            ulong output = DuplicateMapping(SectionNames.Output(transport));
            ulong outputEvent = DuplicateEvent(SectionNames.OutputEvent(transport));
            ulong version = DuplicateReadOnlyMapping(SectionNames.Version(transport));
            ulong identityTag = IdentityTags.OfKey(controller.IdentityKey);
            if (input == 0 || inputEvent == 0)
                throw new InvalidOperationException("could not duplicate the controller's shared sections");

            if (controller.UsbAudio != null)
                plugged.Audio = AudioBridge.TryCreate(serial, controller.UsbAudio, DuplicateForParent);

            _plugged[serial] = plugged;
            int index = checked((int)serial);
            return Ok(w =>
            {
                w.WriteNumber("index", index);
                w.WriteNumber("transportIndex", transport);
                w.WriteNumber("input", input);
                w.WriteNumber("inputEvent", inputEvent);
                w.WriteNumber("companionEvent", companionEvent);
                w.WriteNumber("output", output);
                w.WriteNumber("outputEvent", outputEvent);
                w.WriteNumber("version", version);
                w.WriteNumber("identityTag", identityTag);
                w.WriteBoolean("usbip", profile.RequiresUsbipBackend);
                w.WriteBoolean("companion", companion);
                if (double.IsFinite(TimeoutScale)) w.WriteNumber("timeoutScale", TimeoutScale);
                plugged.Audio?.WriteHandshake(w);
            });
        }
        catch
        {
            plugged.Dispose();
            throw;
        }
    }

    private string Unplug(uint serial)
    {
        if (_plugged.Remove(serial, out PluggedController? plugged)) plugged.Dispose();
        return Ok(_ => { });
    }

    public void Dispose()
    {
        lock (this)
        {
            if (_disposed) return;
            _disposed = true;
        }
        foreach (PluggedController plugged in _plugged.Values) plugged.Dispose();
        _plugged.Clear();
        try { _ctx.Dispose(); }
        catch { }
    }

    private ulong DuplicateMapping(string name)
    {
        nint local = NativeMethods.OpenFileMappingW(NativeMethods.FILE_MAP_READ | NativeMethods.FILE_MAP_WRITE, false, name);
        return DuplicateIntoParent(local, closeLocal: true);
    }

    private ulong DuplicateReadOnlyMapping(string name)
    {
        nint local = NativeMethods.OpenFileMappingW(NativeMethods.FILE_MAP_READ, false, name);
        return DuplicateIntoParent(local, closeLocal: true);
    }

    private ulong DuplicateEvent(string name)
    {
        nint local = NativeMethods.OpenEventW(NativeMethods.EVENT_MODIFY_STATE | NativeMethods.SYNCHRONIZE, false, name);
        return DuplicateIntoParent(local, closeLocal: true);
    }

    private ulong DuplicateForParent(nint local) => DuplicateIntoParent(local, closeLocal: false);

    private ulong DuplicateIntoParent(nint local, bool closeLocal)
    {
        if (local == 0) return 0;
        try
        {
            nint parent = NativeMethods.OpenProcess(NativeMethods.PROCESS_DUP_HANDLE, false, _parentPid);
            if (parent == 0) return 0;
            try
            {
                return NativeMethods.DuplicateHandle(NativeMethods.GetCurrentProcess(), local, parent, out nint remote, 0,
                                                     false, NativeMethods.DUPLICATE_SAME_ACCESS)
                    ? (ulong)remote
                    : 0;
            }
            finally
            {
                NativeMethods.CloseHandle(parent);
            }
        }
        finally
        {
            if (closeLocal) NativeMethods.CloseHandle(local);
        }
    }

    private static string Ok(Action<Utf8JsonWriter> body)
    {
        using var stream = new MemoryStream();
        using (var w = new Utf8JsonWriter(stream))
        {
            w.WriteStartObject();
            w.WriteBoolean("ok", true);
            body(w);
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }

    private static string Error(string message)
    {
        using var stream = new MemoryStream();
        using (var w = new Utf8JsonWriter(stream))
        {
            w.WriteStartObject();
            w.WriteBoolean("ok", false);
            w.WriteString("error", message);
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }
}

internal sealed class PluggedController : IDisposable
{
    public PluggedController(HMController controller)
    {
        Controller = controller;
    }

    public HMController Controller { get; }
    public FrameRelay? Relay { get; set; }
    public AudioBridge? Audio { get; set; }

    public void Dispose()
    {
        try { Audio?.Dispose(); }
        catch { }
        try { Relay?.Dispose(); }
        catch { }
        try { Controller.Dispose(); }
        catch { }
    }
}
