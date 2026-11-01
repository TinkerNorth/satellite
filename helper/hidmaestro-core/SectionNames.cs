// SPDX-License-Identifier: LGPL-3.0-or-later
namespace Satellite.HmHelper.Core;

public static class SectionNames
{
    public static string Input(int transportIndex) => Name("Input", transportIndex);

    public static string InputEvent(int transportIndex) => Name("InputEvent", transportIndex);

    public static string CompanionInputEvent(int transportIndex) => Name("CompanionInputEvent", transportIndex);

    public static string Output(int transportIndex) => Name("Output", transportIndex);

    public static string OutputEvent(int transportIndex) => Name("OutputEvent", transportIndex);

    public static IReadOnlyList<string> Opened(int transportIndex, bool xbox)
    {
        var names = new List<string> { Input(transportIndex), InputEvent(transportIndex) };
        if (xbox) names.Add(CompanionInputEvent(transportIndex));
        names.Add(Output(transportIndex));
        names.Add(OutputEvent(transportIndex));
        return names;
    }

    public static string? FirstUnknown(IReadOnlyList<string> opened, IReadOnlyList<string> sdkNames) =>
        opened.FirstOrDefault(name => !sdkNames.Contains(name, StringComparer.Ordinal));

    private static string Name(string kind, int transportIndex)
    {
        if (transportIndex < 0)
            throw new ArgumentOutOfRangeException(nameof(transportIndex), transportIndex, "a transport index is not negative");
        return $@"Global\HIDMaestro{kind}{transportIndex}";
    }
}
