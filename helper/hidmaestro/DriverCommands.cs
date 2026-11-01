// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using HIDMaestro;

namespace Satellite.HmHelper;

internal static class DriverCommands
{
    public const int ExitRemoved = 0;
    public const int ExitFailed = 1;
    public const int ExitBlocked = 2;
    public const int ExitRemovedRestartPending = 3;

    private static readonly Regex DriverVerLine =
        new(@"^\s*DriverVer\s*=\s*[0-9/]+\s*,\s*([0-9]+(?:\.[0-9]+)*)", RegexOptions.Compiled);

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
        Console.WriteLine($"driver {EmbeddedDriverVersion(sdk)}");
        return 0;
    }

    private static string EmbeddedDriverVersion(Assembly sdk)
    {
        string[] infs = sdk.GetManifestResourceNames()
            .Where(n => n.EndsWith("hidmaestro.inf", StringComparison.OrdinalIgnoreCase))
            .ToArray();
        string arch = $".{RuntimeInformation.ProcessArchitecture}.";
        string name = infs.FirstOrDefault(n => n.Contains(arch, StringComparison.OrdinalIgnoreCase))
            ?? infs.FirstOrDefault()
            ?? throw new InvalidOperationException("the SDK embeds no hidmaestro.inf resource");
        using Stream inf = sdk.GetManifestResourceStream(name)
            ?? throw new InvalidOperationException($"{name} could not be opened");
        using var reader = new StreamReader(inf, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
        while (reader.ReadLine() is string line)
        {
            Match match = DriverVerLine.Match(line);
            if (match.Success) return match.Groups[1].Value;
        }
        throw new InvalidOperationException($"{name} has no DriverVer line");
    }
}
