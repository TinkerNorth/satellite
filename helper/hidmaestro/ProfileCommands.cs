// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Text;
using System.Text.Json;
using HIDMaestro;
using HIDMaestro.Internal;
using Satellite.HmHelper.Core;

namespace Satellite.HmHelper;

internal static class ProfileCommands
{
    private static readonly string[] NativeProfiles = { "xbox-360-wired" };

    public static string InstalledDirectory => Path.Combine(AppContext.BaseDirectory, SatelliteProfiles.DirectoryName);

    public static int Export(string outDir, string catalogDir)
    {
        Directory.CreateDirectory(outDir);
        ProfileDatabase catalog = ProfileDatabase.LoadEmbedded();
        foreach (string id in SatelliteProfiles.Copies)
        {
            string json = SatelliteProfiles.SourceOf(id) == CopySource.Sdk
                ? SdkJson(catalog, id)
                : File.ReadAllText(Path.Combine(catalogDir, SatelliteProfiles.CatalogFileName(id)));
            string path = Path.Combine(outDir, SatelliteProfiles.FileName(id));
            File.WriteAllText(path, SatelliteProfiles.Transform(json, id), new UTF8Encoding(false));
            Console.WriteLine($"exported {path}");
        }
        return 0;
    }

    private static string SdkJson(ProfileDatabase catalog, string id)
    {
        ControllerProfile profile = catalog.All.FirstOrDefault(p => p.Id == id)
            ?? throw new InvalidOperationException($"the HIDMaestro catalog has no profile '{id}'");
        return JsonSerializer.Serialize(profile, typeof(ControllerProfile), HMLayoutJsonOptions.Default);
    }

    public static int Check(string dir)
    {
        using var ctx = new HMContext();
        ProfileSet profiles = ProfileSet.Load(ctx, dir);
        int failures = 0;
        foreach (string issue in profiles.Issues)
        {
            Console.Error.WriteLine($"FAIL {issue}");
            failures++;
        }
        foreach (string id in SatelliteProfiles.Copies)
        {
            HMProfile? copy = profiles.Resolve(ctx, id, out string? refusal);
            string? problem = CopyProblem(copy, ctx.GetProfile(id), refusal);
            if (problem == null)
            {
                Console.WriteLine($"ok   {SatelliteProfiles.CopyId(id)}: {copy!.Name}, builds its own reports: {copy.BuildsItsOwnReports}, {Describe(copy.DeviceModelSource)}");
                continue;
            }
            Console.Error.WriteLine($"FAIL {SatelliteProfiles.CopyId(id)}: {problem}");
            failures++;
        }
        foreach (string id in NativeProfiles)
        {
            HMProfile? profile = profiles.Resolve(ctx, id, out string? refusal);
            if (profile == null)
            {
                Console.Error.WriteLine($"FAIL {id}: {refusal}");
                failures++;
                continue;
            }
            Console.WriteLine($"ok   {id}: {profile.Name}, builds its own reports: {profile.BuildsItsOwnReports}");
        }
        Console.WriteLine(failures == 0 ? "profiles: ok" : $"profiles: {failures} problem(s)");
        return failures == 0 ? 0 : 1;
    }

    private static string Describe(IReadOnlyDictionary<string, string> sources) =>
        string.Join(", ", sources.Select(pair => $"{pair.Key} {pair.Value}"));

    private static string? CopyProblem(HMProfile? copy, HMProfile? original, string? refusal)
    {
        if (copy == null) return refusal ?? "not loaded";
        if (original == null) return "the catalog profile it copies is not loaded";
        if (copy.DeviceModelProblem != null) return $"device model problem: {copy.DeviceModelProblem}";
        if (!copy.HasDeviceModel) return "no device model";
        if (copy.BuildsItsOwnReports != original.BuildsItsOwnReports) return "builds its own reports differently from the catalog profile";
        if (copy.DescriptorHex != original.DescriptorHex) return "descriptor differs from the catalog's";
        if (copy.VendorId != original.VendorId || copy.ProductId != original.ProductId) return "VID/PID differ from the catalog's";
        if (copy.Connection != original.Connection) return "the connection differs from the catalog's";
        if (copy.RequiresUsbipBackend != original.RequiresUsbipBackend) return "the USB transport need differs from the catalog's";
        if (copy.XInput?.Via != original.XInput?.Via) return "the XInput presentation differs from the catalog's";
        if (copy.InputReportSize != original.InputReportSize) return "the input report size differs from the catalog's";
        return null;
    }
}

internal sealed class ProfileSet
{
    private ProfileSet(string directory, IReadOnlyList<string> issues)
    {
        Directory = directory;
        Issues = issues;
    }

    public string Directory { get; }
    public IReadOnlyList<string> Issues { get; }

    public static ProfileSet Load(HMContext ctx, string directory)
    {
        ctx.LoadDefaultProfiles();
        var issues = new List<string>();
        if (System.IO.Directory.Exists(directory))
        {
            ctx.LoadProfilesFromDirectory(directory);
            foreach (HMProfileLoadIssue issue in ctx.ProfileLoadIssues)
                issues.Add($"{issue.Kind} {issue.File} {issue.JsonPath}: {issue.Reason}");
        }
        else
        {
            issues.Add($"the profile directory {directory} does not exist");
        }
        return new ProfileSet(directory, issues);
    }

    public HMProfile? Resolve(HMContext ctx, string catalogId, out string? refusal)
    {
        refusal = null;
        string id = SatelliteProfiles.ResolveId(catalogId);
        HMProfile? profile = ctx.GetProfile(id);
        if (profile != null) return profile;
        if (!SatelliteProfiles.NeedsCopy(catalogId))
        {
            refusal = $"unknown profile '{catalogId}'";
            return null;
        }
        refusal = $"Satellite's copy '{id}' of the '{catalogId}' profile is not loaded from {Directory}";
        if (Issues.Count > 0) refusal += ": " + string.Join("; ", Issues);
        return null;
    }
}
