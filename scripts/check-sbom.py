#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Fail a components SBOM (scripts/write-sbom.py) that does not name what its
artifact ships, at the versions THIRD_PARTY.md states.

THIRD_PARTY.md's summary table is the list of what ships; its Version column
is held to the pins by scripts/check-version-consistency.sh. Each lane must
name the components THIRD_PARTY.md says reach that artifact, and every
package the document names that THIRD_PARTY.md also lists must carry the
table's version (a vcpkg port's version may carry its port revision after a
#).

    scripts/check-sbom.py --lane windows|appimage|deb|rpm|macos [--sentry] <document>
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# THIRD_PARTY.md row name -> the package name write-sbom.py gives it.
NAMES = {
    "cpp-httplib": "cpp-httplib",
    "nlohmann/json": "nlohmann-json",
    "libsodium": "libsodium",
    "opus": "opus",
    "OpenSSL": "openssl",
    "sentry-native": "sentry-native",
    "HIDMaestro": "HIDMaestro",
    "ViGEmBus": "ViGEmBus",
}

EVERY_LANE = ["cpp-httplib", "nlohmann-json"]
LANE = {
    "windows": ["libsodium", "openssl", "opus", "HIDMaestro", "ViGEmBus", "usbip-win2", "ViGEmClient"],
    "appimage": [],
    "deb": [],
    "rpm": [],
    "macos": [],
}


def third_party_versions():
    versions = {}
    with open(os.path.join(ROOT, "THIRD_PARTY.md"), encoding="utf-8") as handle:
        for line in handle:
            match = re.match(r"^\| \[([^\]]+)\]\([^)]*\) \| ([^|]+) \|", line)
            if match and match.group(1) in NAMES:
                versions[NAMES[match.group(1)]] = match.group(2).strip().split()[0]
    return versions


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lane", required=True, choices=sorted(LANE))
    parser.add_argument("--sentry", action="store_true")
    parser.add_argument("document")
    args = parser.parse_args()

    with open(args.document, encoding="utf-8") as handle:
        packages = json.load(handle)["packages"]
    named = {}
    for package in packages:
        named.setdefault(package["name"], []).append(package["versionInfo"].split("#")[0])

    required = list(EVERY_LANE) + LANE[args.lane] + (["sentry-native"] if args.sentry else [])
    if args.lane == "appimage" and len(packages) <= 1 + len(EVERY_LANE):
        required.append("the AppImage's bundled libraries")
    problems = [f"{name} is missing" for name in required if name not in named]

    for name, wanted in third_party_versions().items():
        for got in named.get(name, []):
            if got != wanted:
                problems.append(f"{name} is {got}, THIRD_PARTY.md says {wanted}")

    if problems:
        for problem in problems:
            print(f"::error::check-sbom: {args.document}: {problem}")
        sys.exit(1)
    print(f"check-sbom: {args.document} names {len(packages)} packages, every shipped component at THIRD_PARTY.md's version")


if __name__ == "__main__":
    main()
