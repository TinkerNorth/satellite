// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Runtime.InteropServices;
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
            " | install-driver | remove-driver | cleanup>");
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

        using var reader = new StreamReader(pipe, new UTF8Encoding(false));
        using var writer = new StreamWriter(pipe, new UTF8Encoding(false)) { AutoFlush = true };

        while (true)
        {
            string? line = reader.ReadLine();
            if (line == null) break; // satellite closed the pipe
            string response = session.Handle(line, out bool quit);
            writer.Write(response);
            writer.Write('\n');
            if (quit) break;
        }
        return 0;
    }

    private sealed class Session : IDisposable
    {
        private readonly int _parentPid;
        private readonly HMContext _ctx = new();
        private readonly Dictionary<uint, HMController> _controllers = new();
        private bool _initialized;
        private bool _disposed;

        public Session(int parentPid) => _parentPid = parentPid;

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
                        return Ok(w => w.WriteString("helper", "satellite-hm-helper"));
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
            if (_controllers.TryGetValue(serial, out HMController? stale))
            {
                stale.Dispose();
                _controllers.Remove(serial);
            }

            HMProfile? profile = _ctx.GetProfile(profileId);
            if (profile == null) return Error($"unknown profile '{profileId}'");

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

                _controllers[serial] = controller;
                return Ok(w =>
                {
                    w.WriteNumber("index", index);
                    w.WriteNumber("input", input);
                    w.WriteNumber("inputEvent", inputEvent);
                    w.WriteNumber("companionEvent", companionEvent);
                    w.WriteNumber("output", output);
                    w.WriteNumber("outputEvent", outputEvent);
                });
            }
            catch
            {
                controller.Dispose();
                throw;
            }
        }

        private string Unplug(uint serial)
        {
            if (!_controllers.TryGetValue(serial, out HMController? controller))
                return Ok(_ => { });
            _controllers.Remove(serial);
            controller.Dispose();
            return Ok(_ => { });
        }

        public void Dispose()
        {
            lock (this)
            {
                if (_disposed) return;
                _disposed = true;
            }
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
            return DuplicateIntoParent(local);
        }

        private ulong DuplicateEvent(string name)
        {
            nint local = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, false, name);
            return DuplicateIntoParent(local);
        }

        private ulong DuplicateIntoParent(nint local)
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
                CloseHandle(local);
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
}
