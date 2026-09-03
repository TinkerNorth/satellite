// SPDX-License-Identifier: LGPL-3.0-or-later
using System.IO.MemoryMappedFiles;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using HIDMaestro;

namespace Satellite.HmHelper;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            return args.Length switch
            {
                > 0 when args[0] == "serve" => Serve(ParseOption(args, "--pipe"),
                                                     ParsePid(args)),
                > 0 when args[0] == "service" => ServiceHost.Run(),
                > 0 when args[0] == "broker" => RunBrokerConsole(),
                > 0 when args[0] == "install-driver" => InstallDriver(),
                > 0 when args[0] == "remove-driver" => RemoveDriver(),
                > 0 when args[0] == "cleanup" => Cleanup(),
                _ => Usage(),
            };
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"satellite-hm-helper: {ex.Message}");
            return 1;
        }
    }

    private static int Usage()
    {
        Console.Error.WriteLine(
            "usage: satellite-hm-helper <serve --pipe <name> --parent-pid <pid>" +
            " | service | broker | install-driver | remove-driver | cleanup>");
        return 2;
    }

    private static string ParseOption(string[] args, string name)
    {
        for (int i = 1; i + 1 < args.Length; i++)
        {
            if (args[i] == name) return args[i + 1];
        }
        throw new ArgumentException($"missing {name}");
    }

    private static int ParsePid(string[] args) => int.Parse(ParseOption(args, "--parent-pid"));

    // Installer-time (already elevated): certificate + sign + pnputil deploy.
    // Idempotent; a same-version reinstall is a fast no-op inside the SDK.
    private static int InstallDriver()
    {
        using var ctx = new HMContext();
        ctx.InstallDriver();
        Console.WriteLine("HIDMaestro driver installed.");
        return 0;
    }

    // Uninstaller-time: full teardown including driver packages and the
    // HKLM\SOFTWARE\HIDMaestro configuration.
    private static int RemoveDriver()
    {
        HMContext.RemoveAllVirtualControllers();
        Console.WriteLine("HIDMaestro virtual devices and driver packages removed.");
        return 0;
    }

    // Defensive orphan sweep that keeps the installed driver packages.
    private static int Cleanup()
    {
        HMContext.RemoveAllVirtualControllers(preserveInstall: true);
        Console.WriteLine("HIDMaestro orphan sweep complete.");
        return 0;
    }

    private static int Serve(string pipePath, int parentPid)
    {
        const string prefix = @"\\.\pipe\";
        string pipeName = pipePath.StartsWith(prefix, StringComparison.Ordinal)
            ? pipePath[prefix.Length..]
            : pipePath;

        using var parent = System.Diagnostics.Process.GetProcessById(parentPid);
        using var session = new Session(parentPid);

        // A dead parent must never leave virtual pads plugged: tear down and
        // exit the moment satellite goes away, however it went away.
        parent.EnableRaisingEvents = true;
        parent.Exited += (_, _) =>
        {
            session.Dispose();
            Environment.Exit(0);
        };

        using var pipe = new System.IO.Pipes.NamedPipeClientStream(
            ".", pipeName, System.IO.Pipes.PipeDirection.InOut);
        pipe.Connect(30000);

        Pump(pipe, session);
        return 0;
    }

    private static void Pump(Stream pipe, Session session)
    {
        using var reader = new StreamReader(pipe, new UTF8Encoding(false));
        using var writer = new StreamWriter(pipe, new UTF8Encoding(false)) { AutoFlush = true };

        while (true)
        {
            string? line;
            try
            {
                line = reader.ReadLine();
            }
            catch (IOException)
            {
                break;
            }
            catch (ObjectDisposedException)
            {
                break;
            }
            if (line == null) break;
            string response = session.Handle(line, out bool quit);
            try
            {
                writer.Write(response);
                writer.Write('\n');
            }
            catch (IOException)
            {
                break;
            }
            if (quit) break;
        }
    }

    private static int RunBrokerConsole()
    {
        var broker = new Broker(idleExit: false);
        Console.CancelKeyPress += (_, e) =>
        {
            e.Cancel = true;
            broker.Stop();
        };
        Console.Error.WriteLine($"satellite-hm-helper: broker listening on \\\\.\\pipe\\{Broker.PipeName}");
        broker.Run();
        return 0;
    }

    internal sealed class Broker
    {
        public const string PipeName = "satellite-hm-broker";
        private static readonly TimeSpan IdleExitAfter = TimeSpan.FromMinutes(5);

        private readonly bool _idleExit;
        private readonly string _expectedClient;
        private readonly CancellationTokenSource _stop = new();
        private readonly object _lock = new();
        private bool _busy;
        private Timer? _idle;

        public Broker(bool idleExit)
        {
            _idleExit = idleExit;
            string dir = Path.GetDirectoryName(Environment.ProcessPath ?? "") ?? AppContext.BaseDirectory;
            _expectedClient = Path.GetFullPath(Path.Combine(dir, "satellite.exe"));
        }

        public void Stop()
        {
            _stop.Cancel();
        }

        public void Run()
        {
            ArmIdleTimer();
            while (!_stop.IsCancellationRequested)
            {
                NamedPipeServerStream pipe;
                try
                {
                    pipe = CreateServerPipe();
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine($"satellite-hm-helper: pipe create failed: {ex.Message}");
                    if (_stop.Token.WaitHandle.WaitOne(1000)) break;
                    continue;
                }
                try
                {
                    pipe.WaitForConnectionAsync(_stop.Token).GetAwaiter().GetResult();
                }
                catch (OperationCanceledException)
                {
                    pipe.Dispose();
                    break;
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine($"satellite-hm-helper: accept failed: {ex.Message}");
                    pipe.Dispose();
                    continue;
                }
                var t = new Thread(() => ServeClient(pipe)) { IsBackground = true, Name = "hm-broker-client" };
                t.Start();
            }
        }

        private NamedPipeServerStream CreateServerPipe()
        {
            var security = new PipeSecurity();
            security.AddAccessRule(new PipeAccessRule(
                new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
                PipeAccessRights.FullControl, AccessControlType.Allow));
            security.AddAccessRule(new PipeAccessRule(
                new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null),
                PipeAccessRights.FullControl, AccessControlType.Allow));
            security.AddAccessRule(new PipeAccessRule(
                new SecurityIdentifier(WellKnownSidType.InteractiveSid, null),
                PipeAccessRights.ReadWrite, AccessControlType.Allow));
            using (var self = WindowsIdentity.GetCurrent())
            {
                if (self.User != null)
                    security.AddAccessRule(new PipeAccessRule(self.User, PipeAccessRights.FullControl,
                                                              AccessControlType.Allow));
            }
            return NamedPipeServerStreamAcl.Create(
                PipeName, PipeDirection.InOut, NamedPipeServerStream.MaxAllowedServerInstances,
                PipeTransmissionMode.Byte, PipeOptions.Asynchronous, 64 * 1024, 64 * 1024, security);
        }

        private void ServeClient(NamedPipeServerStream pipe)
        {
            using (pipe)
            {
                int pid = 0;
                string? reason = Authorize(pipe, out pid);
                if (reason != null)
                {
                    WriteLine(pipe, Error(reason));
                    return;
                }

                lock (_lock)
                {
                    if (_busy)
                    {
                        WriteLine(pipe, Error("busy"));
                        return;
                    }
                    _busy = true;
                    _idle?.Change(Timeout.Infinite, Timeout.Infinite);
                }

                try
                {
                    using var client = System.Diagnostics.Process.GetProcessById(pid);
                    using var session = new Session(pid, broker: true);
                    client.EnableRaisingEvents = true;
                    client.Exited += (_, _) =>
                    {
                        try { pipe.Dispose(); } catch { /* unblocks the reader */ }
                    };
                    if (client.HasExited) return;
                    Pump(pipe, session);
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine($"satellite-hm-helper: session ended: {ex.Message}");
                }
                finally
                {
                    lock (_lock)
                    {
                        _busy = false;
                    }
                    ArmIdleTimer();
                }
            }
        }

        private string? Authorize(NamedPipeServerStream pipe, out int pid)
        {
            pid = 0;
            if (!GetNamedPipeClientProcessId(pipe.SafePipeHandle.DangerousGetHandle(), out uint upid))
                return "client pid unavailable";
            pid = checked((int)upid);
            if (!ProcessIdToSessionId(upid, out uint session) || session == 0)
                return "client not interactive";
            string? path = ImagePath(upid);
            if (path == null) return "client image unavailable";
            if (!string.Equals(Path.GetFullPath(path), _expectedClient, StringComparison.OrdinalIgnoreCase))
                return "client not satellite.exe";
            return null;
        }

        private static string? ImagePath(uint pid)
        {
            nint h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, checked((int)pid));
            if (h == 0) return null;
            try
            {
                var sb = new StringBuilder(32768);
                int len = sb.Capacity;
                return QueryFullProcessImageNameW(h, 0, sb, ref len) ? sb.ToString(0, len) : null;
            }
            finally
            {
                CloseHandle(h);
            }
        }

        private static void WriteLine(Stream pipe, string line)
        {
            try
            {
                byte[] bytes = Encoding.UTF8.GetBytes(line + "\n");
                pipe.Write(bytes, 0, bytes.Length);
                pipe.Flush();
            }
            catch { /* client gone */ }
        }

        private void ArmIdleTimer()
        {
            if (!_idleExit) return;
            lock (_lock)
            {
                _idle ??= new Timer(_ =>
                {
                    lock (_lock)
                    {
                        if (_busy) return;
                    }
                    Stop();
                }, null, Timeout.Infinite, Timeout.Infinite);
                _idle.Change(IdleExitAfter, Timeout.InfiniteTimeSpan);
            }
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

        private const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetNamedPipeClientProcessId(nint pipe, out uint clientPid);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool ProcessIdToSessionId(uint pid, out uint sessionId);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern nint OpenProcess(uint access, bool inherit, int pid);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern bool QueryFullProcessImageNameW(nint process, uint flags, StringBuilder name,
                                                              ref int size);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(nint handle);
    }

    internal static class ServiceHost
    {
        public const string ServiceName = "SatelliteHmBroker";

        private const int SERVICE_WIN32_OWN_PROCESS = 0x10;
        private const int SERVICE_STOPPED = 1;
        private const int SERVICE_START_PENDING = 2;
        private const int SERVICE_STOP_PENDING = 3;
        private const int SERVICE_RUNNING = 4;
        private const int SERVICE_ACCEPT_STOP = 1;
        private const int SERVICE_ACCEPT_SHUTDOWN = 4;
        private const int SERVICE_CONTROL_STOP = 1;
        private const int SERVICE_CONTROL_INTERROGATE = 4;
        private const int SERVICE_CONTROL_SHUTDOWN = 5;
        private const int ERROR_CALL_NOT_IMPLEMENTED = 120;
        private const int ERROR_FAILED_SERVICE_CONTROLLER_CONNECT = 1063;

        [StructLayout(LayoutKind.Sequential)]
        private struct SERVICE_STATUS
        {
            public int dwServiceType;
            public int dwCurrentState;
            public int dwControlsAccepted;
            public int dwWin32ExitCode;
            public int dwServiceSpecificExitCode;
            public int dwCheckPoint;
            public int dwWaitHint;
        }

        private delegate void ServiceMainProc(int argc, nint argv);
        private delegate int HandlerExProc(int control, int eventType, nint eventData, nint context);

        private static ServiceMainProc? s_main;
        private static HandlerExProc? s_handler;
        private static nint s_status;
        private static Broker? s_broker;

        public static int Run()
        {
            s_main = ServiceMain;
            nint name = Marshal.StringToHGlobalUni(ServiceName);
            nint table = Marshal.AllocHGlobal(nint.Size * 4);
            Marshal.WriteIntPtr(table, 0, name);
            Marshal.WriteIntPtr(table, nint.Size, Marshal.GetFunctionPointerForDelegate(s_main));
            Marshal.WriteIntPtr(table, nint.Size * 2, 0);
            Marshal.WriteIntPtr(table, nint.Size * 3, 0);
            if (!StartServiceCtrlDispatcherW(table))
            {
                int err = Marshal.GetLastWin32Error();
                Console.Error.WriteLine(err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT
                    ? "satellite-hm-helper: 'service' must be started by the Service Control Manager; use 'broker' to run in a console."
                    : $"satellite-hm-helper: StartServiceCtrlDispatcher failed ({err})");
                return 1;
            }
            return 0;
        }

        private static void ServiceMain(int argc, nint argv)
        {
            s_handler = Handler;
            s_status = RegisterServiceCtrlHandlerExW(ServiceName, s_handler, 0);
            if (s_status == 0) return;
            Report(SERVICE_START_PENDING, 0, 3000);
            s_broker = new Broker(idleExit: true);
            Report(SERVICE_RUNNING, SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN, 0);
            try
            {
                s_broker.Run();
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"satellite-hm-helper: broker failed: {ex.Message}");
            }
            Report(SERVICE_STOPPED, 0, 0);
        }

        private static int Handler(int control, int eventType, nint eventData, nint context)
        {
            switch (control)
            {
                case SERVICE_CONTROL_STOP:
                case SERVICE_CONTROL_SHUTDOWN:
                    Report(SERVICE_STOP_PENDING, 0, 5000);
                    s_broker?.Stop();
                    return 0;
                case SERVICE_CONTROL_INTERROGATE:
                    return 0;
                default:
                    return ERROR_CALL_NOT_IMPLEMENTED;
            }
        }

        private static void Report(int state, int controls, int waitHintMs)
        {
            var status = new SERVICE_STATUS
            {
                dwServiceType = SERVICE_WIN32_OWN_PROCESS,
                dwCurrentState = state,
                dwControlsAccepted = controls,
                dwWaitHint = waitHintMs,
            };
            SetServiceStatus(s_status, ref status);
        }

        [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern bool StartServiceCtrlDispatcherW(nint table);

        [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern nint RegisterServiceCtrlHandlerExW(string name, HandlerExProc handler, nint context);

        [DllImport("advapi32.dll", SetLastError = true)]
        private static extern bool SetServiceStatus(nint status, ref SERVICE_STATUS serviceStatus);
    }

    // Pipe protocol (one JSON line per request, one per response).
    //
    //   {"op":"hello","protocol":1}
    //     -> {"ok":true,"helper":"satellite-hm-helper","audio":true,"broker":B}
    //        `audio` says this helper can broker controller-audio rings; a
    //        satellite talking to an older helper simply sees it absent and
    //        gets no audio fields on plug. `broker` is true when the answer
    //        comes from the SatelliteHmBroker service rather than a helper
    //        satellite spawned (and elevated) itself.
    //
    //   {"op":"plug","serial":N,"profile":"<id>"}
    //     -> {"ok":true,"index":N,"input":H,"inputEvent":H,"companionEvent":H,
    //         "output":H,"outputEvent":H,
    //         // present only when the profile materialized a USB-audio
    //         // function (the composite personas), absent otherwise:
    //         "speakerAudio":H,"speakerAudioEvent":H,
    //         "micAudio":H,"micAudioEvent":H,
    //         "speakerChannels":N,"speakerRateHz":N,
    //         "micChannels":N,"micRateHz":N}
    //
    // Every H is a handle already duplicated into the satellite process, which
    // owns it. The audio sections carry PCM in the ring layout pinned by
    // src/platform/windows/hidmaestro_audio_wire.h; the pipe itself never
    // carries samples.
    private sealed class Session : IDisposable
    {
        private readonly int _parentPid;
        private readonly bool _broker;
        private readonly HMContext _ctx = new();
        private readonly Dictionary<uint, HMController> _controllers = new();
        private readonly Dictionary<uint, AudioBridge> _audio = new();
        private bool _initialized;
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

        private void EnsureInitialized()
        {
            if (_initialized) return;
            // Orphans from a crashed prior session would collide with the
            // deterministic serial->index mapping; sweep them first, keeping
            // the installed driver packages for the fast create path.
            HMContext.RemoveAllVirtualControllers(preserveInstall: true);
            _ctx.LoadDefaultProfiles();
            _initialized = true;
        }

        private string Plug(uint serial, string profileId)
        {
            EnsureInitialized();
            DisposeAudio(serial);
            if (_controllers.TryGetValue(serial, out HMController? stale))
            {
                stale.Dispose();
                _controllers.Remove(serial);
            }

            HMProfile? profile = _ctx.GetProfile(profileId);
            if (profile == null) return Error($"unknown profile '{profileId}'");

            // A composite persona is served over HIDMaestro's bundled usbip-win2
            // kernel USB transport, which self-installs on the first composite
            // creation. Doing it here, explicitly, means a blocked or declined
            // install surfaces as a clean plug failure that satellite can retry
            // without audio, instead of a surprise inside CreateControllerAt.
            if (profile.RequiresUsbipBackend && !HMContext.IsUsbipBackendAvailable)
                HMContext.InstallUsbipBackend();

            HMController controller = _ctx.CreateControllerAt(checked((int)serial), profile);
            try
            {
                _ctx.FinalizeNames();
                int index = checked((int)serial); // CreateControllerAt pinned it
                bool xbox = profileId.StartsWith("xbox-", StringComparison.Ordinal);
                ulong input = DuplicateMapping($@"Global\HIDMaestroInput{index}");
                ulong inputEvent = DuplicateEvent($@"Global\HIDMaestroInputEvent{index}");
                ulong companionEvent =
                    xbox ? DuplicateEvent($@"Global\HIDMaestroCompanionInputEvent{index}") : 0;
                ulong output = DuplicateMapping($@"Global\HIDMaestroOutput{index}");
                ulong outputEvent = DuplicateEvent($@"Global\HIDMaestroOutputEvent{index}");
                if (input == 0 || inputEvent == 0)
                    throw new InvalidOperationException(
                        "could not duplicate the controller's shared sections");

                // Audio is best-effort: a persona with no USB-audio function
                // has no UsbAudio, and a ring we cannot hand over leaves the
                // pad working without endpoints rather than failing the plug.
                AudioBridge? audio = null;
                if (controller.UsbAudio != null)
                {
                    audio = AudioBridge.TryCreate(serial, controller.UsbAudio, DuplicateForParent);
                    if (audio != null) _audio[serial] = audio;
                }

                _controllers[serial] = controller;
                return Ok(w =>
                {
                    w.WriteNumber("index", index);
                    w.WriteNumber("input", input);
                    w.WriteNumber("inputEvent", inputEvent);
                    w.WriteNumber("companionEvent", companionEvent);
                    w.WriteNumber("output", output);
                    w.WriteNumber("outputEvent", outputEvent);
                    audio?.WriteHandshake(w);
                });
            }
            catch
            {
                DisposeAudio(serial);
                controller.Dispose();
                throw;
            }
        }

        private string Unplug(uint serial)
        {
            // Audio first: the bridge's frame handler and its drain thread both
            // reference the controller's UsbAudio objects.
            DisposeAudio(serial);
            if (!_controllers.TryGetValue(serial, out HMController? controller))
                return Ok(_ => { });
            _controllers.Remove(serial);
            controller.Dispose();
            return Ok(_ => { });
        }

        private void DisposeAudio(uint serial)
        {
            if (!_audio.TryGetValue(serial, out AudioBridge? bridge)) return;
            _audio.Remove(serial);
            try { bridge.Dispose(); }
            catch { /* best-effort teardown */ }
        }

        public void Dispose()
        {
            lock (this)
            {
                if (_disposed) return;
                _disposed = true;
            }
            foreach (AudioBridge bridge in _audio.Values)
            {
                try { bridge.Dispose(); }
                catch { /* best-effort teardown */ }
            }
            _audio.Clear();
            foreach (HMController controller in _controllers.Values)
            {
                try { controller.Dispose(); }
                catch { /* best-effort teardown */ }
            }
            _controllers.Clear();
            try { _ctx.Dispose(); }
            catch { /* best-effort teardown */ }
        }

        // Objects are opened fresh by name (this process is elevated, so the
        // Administrators ACE grants full access) and duplicated straight into
        // the satellite process, which owns the returned handle.
        private ulong DuplicateMapping(string name)
        {
            nint local = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, false, name);
            return DuplicateIntoParent(local, closeLocal: true);
        }

        private ulong DuplicateEvent(string name)
        {
            nint local = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, false, name);
            return DuplicateIntoParent(local, closeLocal: true);
        }

        // For objects this process created and keeps using (the audio rings and
        // their doorbells): duplicate a second reference into satellite without
        // dropping ours.
        private ulong DuplicateForParent(nint local) =>
            DuplicateIntoParent(local, closeLocal: false);

        private ulong DuplicateIntoParent(nint local, bool closeLocal)
        {
            if (local == 0) return 0;
            try
            {
                nint parent = OpenProcess(PROCESS_DUP_HANDLE, false, _parentPid);
                if (parent == 0) return 0;
                try
                {
                    return DuplicateHandle(GetCurrentProcess(), local, parent, out nint remote, 0,
                                           false, DUPLICATE_SAME_ACCESS)
                        ? (ulong)remote
                        : 0;
                }
                finally
                {
                    CloseHandle(parent);
                }
            }
            finally
            {
                if (closeLocal) CloseHandle(local);
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

        private const uint FILE_MAP_READ = 0x0004;
        private const uint FILE_MAP_WRITE = 0x0002;
        private const uint EVENT_MODIFY_STATE = 0x0002;
        private const uint SYNCHRONIZE = 0x00100000;
        private const uint PROCESS_DUP_HANDLE = 0x0040;
        private const uint DUPLICATE_SAME_ACCESS = 0x0002;

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern nint OpenFileMappingW(uint access, bool inherit, string name);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern nint OpenEventW(uint access, bool inherit, string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern nint OpenProcess(uint access, bool inherit, int pid);

        [DllImport("kernel32.dll")]
        private static extern nint GetCurrentProcess();

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool DuplicateHandle(nint sourceProcess, nint sourceHandle,
                                                   nint targetProcess, out nint targetHandle,
                                                   uint access, bool inherit, uint options);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(nint handle);
    }

    // Controller audio for one composite persona: the pad's own USB-audio
    // endpoints joined to satellite's two shared-memory rings.
    //
    // Everything here is deliberately mechanical — lane selection, a
    // fixed-layout ring write, a ring read, a channel spread — because this
    // process has no test harness. All the logic worth testing (rate
    // conversion between the persona's endpoints and the pinned wire rate,
    // 20 ms windowing, the codec) lives on the satellite side where ctest
    // covers it. The rule the rings encode: the ring always speaks the WIRE's
    // channel layout (stereo out, mono in) at the PERSONA's sample rate.
    //
    // The layout constants below mirror
    // src/platform/windows/hidmaestro_audio_wire.h, which is the source of
    // truth. Satellite maps exactly AudioSectionSize bytes, so a drift here
    // fails the map instead of misreading PCM.
    private sealed class AudioBridge : IDisposable
    {
        private const int RingSlots = 32;
        private const int RingHeaderSize = 8;
        private const int SlotSeqNoOffset = 0;
        private const int SlotSerialOffset = 4;
        private const int SlotSeqOffset = 8;
        private const int SlotSamplesOffset = 10;
        private const int SlotDataOffset = 12;
        private const int SlotDataCapacity = 4096;
        private const int SlotSampleCapacity = SlotDataCapacity / 2;
        private const int SlotSize = SlotDataOffset + SlotDataCapacity;
        private const long AudioSectionSize = RingHeaderSize + (long)RingSlots * SlotSize;

        private const int WireSpeakerChannels = 2;

        // The SDK raises one USB service interval (1 ms) of OUT audio at a
        // time. A doorbell per millisecond would be a thousand cross-process
        // wakeups a second for nothing, since satellite re-windows into 20 ms
        // Opus frames regardless. 240 frames is 5 ms at 48 kHz: 200 wakeups a
        // second, and at most 5 ms of added latency inside a 20 ms window.
        private const int SpeakerFlushFrames = 240;

        // 500 ms safety net behind the doorbell, matching the adapter's ring
        // workers: a missed signal costs latency, never a stuck stream.
        private const int DoorbellTimeoutMs = 500;

        private readonly uint _serial;
        private readonly HMAudioOutput _output;
        private readonly HMMicrophoneInput _microphone;
        private readonly object _speakerLock = new();

        private MemoryMappedFile? _speakerMap;
        private MemoryMappedFile? _micMap;
        private MemoryMappedViewAccessor? _speakerView;
        private MemoryMappedViewAccessor? _micView;
        private EventWaitHandle? _speakerDoorbell;
        private EventWaitHandle? _micDoorbell;
        private EventWaitHandle? _cancel;
        private Thread? _micThread;

        private readonly short[] _speakerPending = new short[SpeakerFlushFrames * WireSpeakerChannels];
        private int _speakerPendingFrames;
        private ushort _speakerSeq;

        private readonly short[] _micSlot = new short[SlotSampleCapacity];
        private byte[] _micBytes = new byte[SlotSampleCapacity * 2];
        private uint _micLastSeq;

        private ulong _speakerHandle;
        private ulong _speakerEventHandle;
        private ulong _micHandle;
        private ulong _micEventHandle;

        private volatile bool _disposed;

        private AudioBridge(uint serial, HMUsbAudio audio)
        {
            _serial = serial;
            _output = audio.Output;
            _microphone = audio.Microphone;
        }

        internal static AudioBridge? TryCreate(uint serial, HMUsbAudio audio,
                                               Func<nint, ulong> duplicate)
        {
            var bridge = new AudioBridge(serial, audio);
            try
            {
                bridge.Start(duplicate);
                return bridge;
            }
            catch
            {
                bridge.Dispose();
                return null; // no endpoints on this pad; the pad itself is fine
            }
        }

        private void Start(Func<nint, ulong> duplicate)
        {
            _speakerMap = MemoryMappedFile.CreateNew(null, AudioSectionSize);
            _micMap = MemoryMappedFile.CreateNew(null, AudioSectionSize);
            _speakerView = _speakerMap.CreateViewAccessor();
            _micView = _micMap.CreateViewAccessor();
            _speakerDoorbell = new EventWaitHandle(false, EventResetMode.AutoReset);
            _micDoorbell = new EventWaitHandle(false, EventResetMode.AutoReset);
            _cancel = new EventWaitHandle(false, EventResetMode.ManualReset);

            _speakerHandle = duplicate(_speakerMap.SafeMemoryMappedFileHandle.DangerousGetHandle());
            _micHandle = duplicate(_micMap.SafeMemoryMappedFileHandle.DangerousGetHandle());
            _speakerEventHandle = duplicate(_speakerDoorbell.SafeWaitHandle.DangerousGetHandle());
            _micEventHandle = duplicate(_micDoorbell.SafeWaitHandle.DangerousGetHandle());
            if (_speakerHandle == 0 || _micHandle == 0 || _speakerEventHandle == 0 ||
                _micEventHandle == 0)
                throw new InvalidOperationException("could not duplicate the audio ring handles");

            _output.FramesReceived += OnOutputFrames;

            // Background so a wedged drain can never hold the process open, and
            // so the plug/unplug control path never waits on audio work.
            _micThread = new Thread(MicLoop) { IsBackground = true, Name = "hm-mic-drain" };
            _micThread.Start();
        }

        internal void WriteHandshake(Utf8JsonWriter w)
        {
            w.WriteNumber("speakerAudio", _speakerHandle);
            w.WriteNumber("speakerAudioEvent", _speakerEventHandle);
            w.WriteNumber("micAudio", _micHandle);
            w.WriteNumber("micAudioEvent", _micEventHandle);
            // What the ring carries, so satellite can rate-convert. Channels
            // are the ENDPOINT's (satellite spreads mono across them on the
            // way in); the rate is the endpoint's on both rings.
            w.WriteNumber("speakerChannels", WireSpeakerChannels);
            w.WriteNumber("speakerRateHz", _output.SampleRateHz);
            w.WriteNumber("micChannels", _microphone.Channels);
            w.WriteNumber("micRateHz", _microphone.SampleRateHz);
        }

        // The game's audio, straight off the emulated pad's OUT endpoint. On a
        // DualSense that is four channels: 1/2 are the speaker/headset lanes
        // and 3/4 are the HD-haptics lanes, which deliberately never cross the
        // wire. Runs on the SDK's own audio pump thread.
        private void OnOutputFrames(HMAudioOutput source, ReadOnlyMemory<byte> pcm)
        {
            if (_disposed) return;
            int channels = source.Channels;
            if (channels < WireSpeakerChannels || source.BitsPerSample != 16) return;

            ReadOnlySpan<byte> bytes = pcm.Span;
            int stride = channels * 2;
            int frames = bytes.Length / stride;
            lock (_speakerLock)
            {
                if (_disposed) return;
                for (int f = 0; f < frames; f++)
                {
                    int b = f * stride;
                    int o = _speakerPendingFrames * WireSpeakerChannels;
                    _speakerPending[o] = unchecked((short)(bytes[b] | (bytes[b + 1] << 8)));
                    _speakerPending[o + 1] = unchecked((short)(bytes[b + 2] | (bytes[b + 3] << 8)));
                    _speakerPendingFrames++;
                    if (_speakerPendingFrames >= SpeakerFlushFrames) FlushSpeakerLocked();
                }
            }
        }

        private void FlushSpeakerLocked()
        {
            if (_speakerPendingFrames <= 0 || _speakerView == null) return;
            WriteSlot(_speakerView, _serial, _speakerSeq, _speakerPending,
                      _speakerPendingFrames * WireSpeakerChannels);
            _speakerSeq++;
            _speakerPendingFrames = 0;
            _speakerDoorbell?.Set();
        }

        private void MicLoop()
        {
            WaitHandle[] waits = { _cancel!, _micDoorbell! };
            while (!_disposed)
            {
                int rc = WaitHandle.WaitAny(waits, DoorbellTimeoutMs);
                if (rc == 0) return; // cancelled
                if (_disposed) return;

                MemoryMappedViewAccessor? view = _micView;
                if (view == null) return;
                while (ReadSlot(view, ref _micLastSeq, _micSlot, out uint slotSerial,
                                out int samples))
                {
                    if (slotSerial != _serial || samples <= 0) continue;
                    SubmitMic(samples);
                }
            }
        }

        // Mono at the endpoint's rate in, whatever the endpoint's channel count
        // wants out. Submitted unconditionally rather than gated on
        // IsStreaming: the SDK's own ring absorbs and drops what nobody is
        // recording, and a silent mic that depends on an alt-setting event
        // having fired is far harder to diagnose than a few wasted memcpys.
        private void SubmitMic(int monoSamples)
        {
            int channels = _microphone.Channels;
            if (channels < 1) channels = 1;
            int needed = monoSamples * channels * 2;
            if (_micBytes.Length < needed) _micBytes = new byte[needed];

            int o = 0;
            for (int i = 0; i < monoSamples; i++)
            {
                byte lo = (byte)(_micSlot[i] & 0xFF);
                byte hi = (byte)((_micSlot[i] >> 8) & 0xFF);
                // The DualSense mic function is declared stereo; the same
                // sample goes to every channel rather than silence on one.
                for (int c = 0; c < channels; c++)
                {
                    _micBytes[o++] = lo;
                    _micBytes[o++] = hi;
                }
            }
            _microphone.Submit(new ReadOnlySpan<byte>(_micBytes, 0, needed));
        }

        // Ring producer. Single writer per ring, so reserving the sequence is a
        // plain increment. Zeroing the slot's SeqNo first marks it in progress:
        // no published batch carries sequence 0, so a reader mid-write sees a
        // value matching no expectation and retries.
        private static void WriteSlot(MemoryMappedViewAccessor view, uint serial, ushort seq,
                                      short[] pcm, int sampleCount)
        {
            if (sampleCount > SlotSampleCapacity) return;
            uint next = view.ReadUInt32(0) + 1;
            view.Write(0, next);

            long slot = RingHeaderSize + (long)((next - 1) % RingSlots) * SlotSize;
            view.Write(slot + SlotSeqNoOffset, (uint)0);
            Thread.MemoryBarrier();

            view.Write(slot + SlotSerialOffset, serial);
            view.Write(slot + SlotSeqOffset, seq);
            view.Write(slot + SlotSamplesOffset, (ushort)sampleCount);
            if (sampleCount > 0) view.WriteArray(slot + SlotDataOffset, pcm, 0, sampleCount);

            Thread.MemoryBarrier();
            view.Write(slot + SlotSeqNoOffset, next);
        }

        // Ring consumer, the mirror of readNextAudioSlot: lap-skip to the
        // oldest still-readable slot, then a seqlock retry around the copy.
        private static bool ReadSlot(MemoryMappedViewAccessor view, ref uint lastSeq, short[] dst,
                                     out uint serial, out int sampleCount)
        {
            serial = 0;
            sampleCount = 0;
            uint head = view.ReadUInt32(0);
            if (head == lastSeq) return false;

            uint nextSeq = lastSeq + 1;
            if (head > nextSeq + RingSlots - 1) nextSeq = head - RingSlots + 1;
            long slot = RingHeaderSize + (long)((nextSeq - 1) % RingSlots) * SlotSize;

            for (int retries = 0; retries < 4; retries++)
            {
                uint before = view.ReadUInt32(slot + SlotSeqNoOffset);
                if (before != nextSeq) return false;
                Thread.MemoryBarrier();

                uint slotSerial = view.ReadUInt32(slot + SlotSerialOffset);
                int samples = view.ReadUInt16(slot + SlotSamplesOffset);
                if (samples > SlotSampleCapacity) samples = SlotSampleCapacity;
                if (samples > 0) view.ReadArray(slot + SlotDataOffset, dst, 0, samples);

                Thread.MemoryBarrier();
                if (view.ReadUInt32(slot + SlotSeqNoOffset) == before)
                {
                    lastSeq = nextSeq;
                    serial = slotSerial;
                    sampleCount = samples;
                    return true;
                }
            }
            return false;
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;

            try { _output.FramesReceived -= OnOutputFrames; }
            catch { /* the controller may already be torn down */ }

            // _disposed is already set, so taking the lock waits out an
            // in-flight frame handler and guarantees no later one runs: the
            // view must not be unmapped underneath a memcpy.
            lock (_speakerLock)
            {
                _speakerView?.Dispose();
                _speakerView = null;
                _speakerMap?.Dispose();
                _speakerMap = null;
                _speakerDoorbell?.Dispose();
                _speakerDoorbell = null;
            }

            _cancel?.Set();
            bool joined = true;
            if (_micThread != null)
            {
                try { joined = _micThread.Join(2000); }
                catch { joined = false; }
                _micThread = null;
            }

            // A drain thread that would not join still holds the mapped view.
            // Unmapping it would be an access violation, so a wedged bridge
            // leaks its 128 KB section instead: bounded, and the process exits
            // with satellite anyway.
            if (joined)
            {
                _micView?.Dispose();
                _micView = null;
                _micMap?.Dispose();
                _micMap = null;
                _micDoorbell?.Dispose();
                _micDoorbell = null;
                _cancel?.Dispose();
                _cancel = null;
            }
        }
    }
}
