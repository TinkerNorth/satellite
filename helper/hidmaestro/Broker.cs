// SPDX-License-Identifier: LGPL-3.0-or-later
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using System.Text.Json;

namespace Satellite.HmHelper;

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
                Program.Pump(pipe, session);
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
