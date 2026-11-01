// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Text;

namespace Satellite.HmHelper;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            return args.Length switch
            {
                > 0 when args[0] == "serve" => Serve(ParseOption(args, "--pipe"), ParsePid(args)),
                > 0 when args[0] == "service" => ServiceHost.Run(),
                > 0 when args[0] == "broker" => RunBrokerConsole(),
                > 0 when args[0] == "install-driver" => DriverCommands.Install(),
                > 0 when args[0] == "remove-driver" => DriverCommands.Remove(HasFlag(args, "--include-usbip")),
                > 0 when args[0] == "cleanup" => DriverCommands.Cleanup(),
                > 0 when args[0] == "sdk-version" => DriverCommands.SdkVersion(),
                > 0 when args[0] == "export-profiles" => ProfileCommands.Export(ParseOption(args, "--out")),
                > 0 when args[0] == "check-profiles" =>
                    ProfileCommands.Check(ParseOptionOr(args, "--dir", ProfileCommands.InstalledDirectory)),
                > 0 when args[0] == "selftest" => SelfTest.Run(ParseOptionOrNull(args, "--log")),
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
            "usage: satellite-hm-helper <serve --pipe <name> --parent-pid <pid> | service | broker" +
            " | install-driver | remove-driver [--include-usbip] | cleanup | sdk-version" +
            " | export-profiles --out <dir> | check-profiles [--dir <dir>] | selftest [--log <file>]>");
        return 2;
    }

    private static string ParseOption(string[] args, string name) =>
        ParseOptionOrNull(args, name) ?? throw new ArgumentException($"missing {name}");

    private static string ParseOptionOr(string[] args, string name, string fallback) =>
        ParseOptionOrNull(args, name) ?? fallback;

    private static string? ParseOptionOrNull(string[] args, string name)
    {
        for (int i = 1; i + 1 < args.Length; i++)
        {
            if (args[i] == name) return args[i + 1];
        }
        return null;
    }

    private static bool HasFlag(string[] args, string name) => args.Skip(1).Contains(name, StringComparer.Ordinal);

    private static int ParsePid(string[] args) => int.Parse(ParseOption(args, "--parent-pid"));

    private static int Serve(string pipePath, int parentPid)
    {
        const string prefix = @"\\.\pipe\";
        string pipeName = pipePath.StartsWith(prefix, StringComparison.Ordinal)
            ? pipePath[prefix.Length..]
            : pipePath;

        using var parent = System.Diagnostics.Process.GetProcessById(parentPid);
        using var session = new Session(parentPid);

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

    internal static void Pump(Stream pipe, Session session)
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
}
