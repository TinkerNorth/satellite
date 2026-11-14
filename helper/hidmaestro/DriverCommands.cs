// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Reflection;
using HIDMaestro;

namespace Satellite.HmHelper;

internal static class DriverCommands
{
    public const int ExitRemoved = 0;
    public const int ExitFailed = 1;
    public const int ExitBlocked = 2;
    public const int ExitRemovedRestartPending = 3;

    public static int Install()
    {
        using var ctx = new HMContext();
        ctx.InstallDriver();
        Console.WriteLine($"HIDMaestro driver: {Describe(ctx.LastDriverInstall)}.");
        return 0;
    }

    public static string Describe(HMDriverInstallOutcome? outcome) => outcome switch
    {
        HMDriverInstallOutcome.AlreadyCurrent => "already current",
        HMDriverInstallOutcome.Installed => "installed",
        HMDriverInstallOutcome.Upgraded => "upgraded",
        HMDriverInstallOutcome.KeptNewer => "kept, a newer driver is installed",
        _ => "deploy reported no outcome",
    };

    public static int Remove(bool includeUsbip)
    {
        HMUninstallReport report = HMUninstall.Run(new HMUninstallOptions { IncludeUsbip = includeUsbip });
        foreach (HMRunningProgram program in report.RunningPrograms)
            Console.WriteLine($"running: {program}");
        foreach (HMUninstallEntry entry in report.Entries)
            Console.WriteLine(entry.ToString());
        Console.WriteLine($"HIDMaestro removal: {report.Status}.");
        return ExitCodeFor(report.Status);
    }

    public static int ExitCodeFor(HMUninstallStatus status) => status switch
    {
        HMUninstallStatus.Removed => ExitRemoved,
        HMUninstallStatus.Previewed => ExitRemoved,
        HMUninstallStatus.RemovedRestartPending => ExitRemovedRestartPending,
        HMUninstallStatus.Blocked => ExitBlocked,
        _ => ExitFailed,
    };

    public static int Cleanup()
    {
        HMContext.RemoveAllVirtualControllers(preserveInstall: true);
        Console.WriteLine("HIDMaestro orphan sweep complete.");
        return 0;
    }

    public static int SdkVersion()
    {
        Assembly sdk = typeof(HMContext).Assembly;
        string release = sdk.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
                         ?? sdk.GetName().Version?.ToString()
                         ?? "unknown";
        Console.WriteLine($"sdk {release}");
        Console.WriteLine($"driver {SdkInf.EmbeddedDriverVersion(sdk)}");
        return 0;
    }
}
