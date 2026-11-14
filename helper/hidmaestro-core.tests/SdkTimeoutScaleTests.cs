// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Globalization;
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class SdkTimeoutScaleTests
{
    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("\t")]
    public void ABlankVariableLeavesTheWaitsUnscaled(string? raw)
    {
        Assert.Equal(1.0, SdkTimeoutScale.Resolve(raw));
    }

    [Theory]
    [InlineData("abc")]
    [InlineData("2,5")]
    [InlineData("0x10")]
    [InlineData("2.5s")]
    public void AValueThatIsNotANumberLeavesTheWaitsUnscaled(string raw)
    {
        Assert.Equal(1.0, SdkTimeoutScale.Resolve(raw));
    }

    [Theory]
    [InlineData("0.05")]
    [InlineData("0.0999")]
    [InlineData("100.01")]
    [InlineData("-2")]
    [InlineData("Infinity")]
    [InlineData("1e400")]
    public void AFactorOutsideTheRangeLeavesTheWaitsUnscaled(string raw)
    {
        Assert.Equal(1.0, SdkTimeoutScale.Resolve(raw));
    }

    [Theory]
    [InlineData("0.1", 0.1)]
    [InlineData("100", 100.0)]
    [InlineData("2", 2.0)]
    [InlineData("2.5", 2.5)]
    [InlineData(" 3 ", 3.0)]
    [InlineData("1e1", 10.0)]
    [InlineData("+4", 4.0)]
    public void AFactorInTheRangeBothEndsIncludedIsTaken(string raw, double factor)
    {
        Assert.Equal(factor, SdkTimeoutScale.Resolve(raw));
    }

    [Fact]
    public void TheNumberIsReadInTheInvariantCultureWhateverTheThreadsCulture()
    {
        // The helper runs globalization-invariant, so a comma-decimal culture
        // is made by hand rather than looked up.
        var commaDecimal = (CultureInfo)CultureInfo.InvariantCulture.Clone();
        commaDecimal.NumberFormat.NumberDecimalSeparator = ",";
        commaDecimal.NumberFormat.NumberGroupSeparator = ".";
        CultureInfo culture = CultureInfo.CurrentCulture;
        try
        {
            CultureInfo.CurrentCulture = commaDecimal;
            Assert.Equal(2.5, SdkTimeoutScale.Resolve("2.5"));
            Assert.Equal(1.0, SdkTimeoutScale.Resolve("2,5"));
        }
        finally
        {
            CultureInfo.CurrentCulture = culture;
        }
    }

    [Fact]
    public void NaNPassesTheRangeCheckAsItDoesInTheSdk()
    {
        // Neither comparison of the SDK's range check holds for NaN, so the
        // SDK takes it (TimeoutScale.cs:78); check-sdk holds this to that.
        Assert.True(double.IsNaN(SdkTimeoutScale.Resolve("NaN")));
    }
}
