// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Reflection.Emit;
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class IlScanTests
{
    private const string Prefix = @"SWD\FAKE\";

    // Stand-ins for the SDK's software-device factory, and for another type
    // with a Create of its own.
    private static class Factory
    {
        public static string Create(string id) => id;
    }

    private static class OtherFactory
    {
        public static string Create(string id) => id;
    }

    private sealed class BuildsInOneMethod
    {
        public string Make(int n) => Factory.Create(Prefix + n);
    }

    private sealed class LoadsInOneMethodCreatesInAnother
    {
        public string Id(int n) => Prefix + n;

        public string Make(int n) => Factory.Create(Id(n));
    }

    private sealed class BuildsForAnotherFactory
    {
        public string Make(int n) => OtherFactory.Create(Prefix + n);
    }

    // A switch, a long and a double ahead of the literal: operands of 4 + 4n,
    // 8 and 8 bytes the walk has to step over.
    private sealed class BuildsAfterWideOperands
    {
        public string Make(int n, long wide, double real)
        {
            string kind = n switch { 0 => "a", 1 => "b", 2 => "c", 3 => "d", _ => "e" };
            long sum = wide + 0x1122334455667788;
            double scaled = real * 2.5;
            return Factory.Create(Prefix + kind + sum + scaled);
        }
    }

    [Fact]
    public void AMethodThatLoadsTheLiteralAndCallsTheFactoryBuildsFromIt()
    {
        Assert.True(IlScan.BuildsFrom(typeof(BuildsInOneMethod), typeof(Factory), "Create", Prefix));
    }

    [Fact]
    public void ALiteralLoadedInOneMethodAndACallMadeInAnotherDoNotCount()
    {
        Assert.False(IlScan.BuildsFrom(typeof(LoadsInOneMethodCreatesInAnother), typeof(Factory), "Create", Prefix));
    }

    [Fact]
    public void AnotherTypesCreateDoesNotCount()
    {
        Assert.False(IlScan.BuildsFrom(typeof(BuildsForAnotherFactory), typeof(Factory), "Create", Prefix));
        Assert.True(IlScan.BuildsFrom(typeof(BuildsForAnotherFactory), typeof(OtherFactory), "Create", Prefix));
    }

    [Fact]
    public void AnotherLiteralDoesNotCount()
    {
        Assert.False(IlScan.BuildsFrom(typeof(BuildsInOneMethod), typeof(Factory), "Create", @"SWD\OTHER\"));
    }

    [Fact]
    public void TheWalkStepsOverWideOperands()
    {
        var codes = IlScan.Instructions(typeof(BuildsAfterWideOperands).GetMethod(nameof(BuildsAfterWideOperands.Make))!)
                          .Select(i => i.Code).ToList();
        Assert.Contains(OpCodes.Switch, codes);
        Assert.Contains(OpCodes.Ldc_I8, codes);
        Assert.Contains(OpCodes.Ldc_R8, codes);
        Assert.Equal(OpCodes.Ret, codes[^1]);
        Assert.True(IlScan.BuildsFrom(typeof(BuildsAfterWideOperands), typeof(Factory), "Create", Prefix));
    }
}
