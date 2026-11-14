// SPDX-License-Identifier: LGPL-3.0-or-later
using Satellite.HmHelper.Core;
using Xunit;

namespace Satellite.HmHelper.Core.Tests;

public sealed class CppConstantsTests
{
    [Fact]
    public void EachIntegerConstexprIsReadWithItsValue()
    {
        const string header = """
            inline constexpr size_t VERSION_SECTION_SIZE = 64;
            inline constexpr size_t VERSION_LAYOUT_OFFSET = 0;                  // SDK
              inline constexpr int32_t SDK_PROTOCOL = 4;
            inline constexpr int VERSION_HALF_DEADLINE_MS = 15000;
            inline constexpr int32_t NEGATIVE = -2;
            """;
        IReadOnlyDictionary<string, long> constants = CppConstants.Parse(header);
        Assert.Equal(64, constants["VERSION_SECTION_SIZE"]);
        Assert.Equal(0, constants["VERSION_LAYOUT_OFFSET"]);
        Assert.Equal(4, constants["SDK_PROTOCOL"]);
        Assert.Equal(15000, constants["VERSION_HALF_DEADLINE_MS"]);
        Assert.Equal(-2, constants["NEGATIVE"]);
        Assert.Equal(5, constants.Count);
    }

    [Fact]
    public void WhatIsNotAnIntegerConstexprIsLeftOut()
    {
        const string header = """
            // inline constexpr size_t COMMENTED = 1;
            constexpr size_t NOT_INLINE = 2;
            inline constexpr const char* TEXT = "x";
            inline constexpr size_t EXPRESSION = 4 * 2;
            static_assert(sizeof(int) == 4);
            """;
        Assert.Empty(CppConstants.Parse(header));
    }

    [Fact]
    public void AConstantDefinedTwiceIsRefused()
    {
        const string header = """
            inline constexpr int32_t SDK_PROTOCOL = 4;
            inline constexpr int32_t SDK_PROTOCOL = 5;
            """;
        Assert.Throws<FormatException>(() => CppConstants.Parse(header));
    }
}
