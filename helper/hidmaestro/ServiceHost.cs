// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Runtime.InteropServices;

namespace Satellite.HmHelper;

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
