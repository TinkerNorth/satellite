// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/package_format.h"

#include <iostream>

#include "test_util.h"

using satellite::PackageFormat;
using satellite::parsePackageFormat;

static void test_the_two_native_formats_are_named() {
    TEST("package format: deb and rpm name their package");
    EXPECT(parsePackageFormat("deb") == PackageFormat::Deb);
    EXPECT(parsePackageFormat("rpm") == PackageFormat::Rpm);
}

static void test_anything_else_is_no_package() {
    TEST("package format: an empty or unknown name is no package");
    EXPECT(parsePackageFormat("") == PackageFormat::None);
    EXPECT(parsePackageFormat("DEB") == PackageFormat::None);
    EXPECT(parsePackageFormat("appimage") == PackageFormat::None);
}

int main() {
    return satellite::test::runMain("test_package_format", [] {
        test_the_two_native_formats_are_named();
        test_anything_else_is_no_package();
    });
}
