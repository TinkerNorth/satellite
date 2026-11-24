#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Write the SPDX bill of materials of what one Satellite artifact carries.

Syft, which the release hardening runs over the release directory, sees an
installer, a .deb, an .rpm, an AppImage or a .dmg and names nothing inside
it but Satellite. This document is written from the build instead, at the
versions the build used, and shipped beside the artifact as
satellite-<lane>.components.spdx.json; the shared _release_harden.yml folds
it into the product SBOM (Syft's sbom-cataloger) and Grype scans the result.

What each lane names:

  every lane   cpp-httplib and nlohmann/json, compiled in from lib/ at the
               versions their headers state (lib/VENDORED.md pins them);
               sentry-native with --sentry (a Linux or macOS release build
               with a DSN)
  windows      every port vcpkg resolved for the target triplet, sentry-native
               and its Crashpad among them, from the vcpkg.spdx.json vcpkg
               writes for it under <vcpkg_installed>/<triplet>/share/<port>/;
               HIDMaestro and ViGEmBus at driver_pins.h's versions, and the
               usbip-win2 driver HIDMaestro embeds; the ViGEm client headers
  appimage     every shared library linuxdeploy copied into the AppImage,
               by the Ubuntu package that owns it on the build host
               (dpkg-query), so the document carries the distribution's
               version of each
  deb, rpm,    nothing else: their libraries are the distribution's
  macos        (dependencies the package manager installs) or Homebrew's
               (the macOS stub links them from /opt/homebrew)

scripts/check-sbom.py holds the document to THIRD_PARTY.md.

    scripts/write-sbom.py --lane windows --out FILE --vcpkg-installed DIR [--sentry]
    scripts/write-sbom.py --lane appimage --out FILE --appdir DIR [--sentry]
    scripts/write-sbom.py --lane deb|rpm|macos --out FILE [--sentry]
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as handle:
        return handle.read()


def define(text, name):
    match = re.search(r"#define\s+%s\s+\"?([^\"\s]+)\"?" % re.escape(name), text)
    if not match:
        sys.exit(f"write-sbom: no #define {name}")
    return match.group(1)


def satellite_version():
    return read("VERSION").strip()


class Document:
    def __init__(self, name):
        self.name = name
        self.packages = []
        self.ids = set()

    def add(self, name, version, license_id, download, purl="", cpe="", supplier="NOASSERTION"):
        base = "SPDXRef-Package-" + re.sub(r"[^A-Za-z0-9.-]", "-", f"{name}-{version}")
        spdx_id = base
        suffix = 2
        while spdx_id in self.ids:
            spdx_id = f"{base}-{suffix}"
            suffix += 1
        self.ids.add(spdx_id)
        refs = []
        if purl:
            refs.append({"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl", "referenceLocator": purl})
        if cpe:
            refs.append({"referenceCategory": "SECURITY", "referenceType": "cpe23Type", "referenceLocator": cpe})
        package = {
            "name": name,
            "SPDXID": spdx_id,
            "versionInfo": version,
            "supplier": supplier,
            "downloadLocation": download,
            "filesAnalyzed": False,
            "licenseConcluded": license_id,
            "licenseDeclared": license_id,
            "copyrightText": "NOASSERTION",
        }
        if refs:
            package["externalRefs"] = refs
        self.packages.append(package)

    def write(self, out):
        version = satellite_version()
        body = {
            "spdxVersion": "SPDX-2.3",
            "dataLicense": "CC0-1.0",
            "SPDXID": "SPDXRef-DOCUMENT",
            "name": self.name,
            "documentNamespace": "",
            "creationInfo": {
                # Fixed, so two builds of the same tree write the same bytes.
                "created": "2026-01-01T00:00:00Z",
                "creators": ["Organization: TinkerNorth", "Tool: scripts/write-sbom.py"],
            },
            "packages": self.packages,
            "relationships": [
                {"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": p["SPDXID"]}
                for p in self.packages
            ],
        }
        digest = hashlib.sha256(json.dumps(self.packages, sort_keys=True).encode()).hexdigest()
        body["documentNamespace"] = f"https://github.com/TinkerNorth/satellite/sbom/{self.name}/{version}/{digest}"
        with open(out, "w", encoding="utf-8") as handle:
            json.dump(body, handle, indent=2, sort_keys=False)
            handle.write("\n")


def add_vendored(doc):
    httplib = define(read("lib/httplib.h"), "CPPHTTPLIB_VERSION")
    doc.add("cpp-httplib", httplib, "MIT", f"https://github.com/yhirose/cpp-httplib/releases/tag/v{httplib}",
            purl=f"pkg:github/yhirose/cpp-httplib@v{httplib}",
            cpe=f"cpe:2.3:a:yhirose:cpp-httplib:{httplib}:*:*:*:*:*:*:*")
    json_hpp = read("lib/nlohmann/json.hpp")
    json_version = ".".join(define(json_hpp, f"NLOHMANN_JSON_VERSION_{part}") for part in ("MAJOR", "MINOR", "PATCH"))
    doc.add("nlohmann-json", json_version, "MIT", f"https://github.com/nlohmann/json/releases/tag/v{json_version}",
            purl=f"pkg:github/nlohmann/json@v{json_version}",
            cpe=f"cpe:2.3:a:json-for-modern-cpp_project:json-for-modern-cpp:{json_version}:*:*:*:*:*:*:*")


def add_sentry(doc):
    port = json.loads(read("vcpkg/ports/sentry-native/vcpkg.json"))
    version = port["version"]
    doc.add("sentry-native", version, "MIT", f"https://github.com/getsentry/sentry-native/releases/tag/{version}",
            purl=f"pkg:github/getsentry/sentry-native@{version}")


def add_windows(doc, vcpkg_installed, triplet):
    found = 0
    # The target triplet only: the host triplet holds build tools (pkgconf)
    # that run during the build and ship in nothing.
    share = os.path.join(vcpkg_installed, triplet, "share")
    if os.path.isdir(share):
        for port in sorted(os.listdir(share)):
            spdx = os.path.join(share, port, "vcpkg.spdx.json")
            if not os.path.isfile(spdx):
                continue
            with open(spdx, encoding="utf-8") as handle:
                data = json.load(handle)
            # vcpkg's document describes the port first; its name and version
            # (with the port revision after a #) are what was built.
            described = data["packages"][0]
            name = described["name"]
            if name.startswith("vcpkg-") or name.lower() == "satellite":
                # Build helpers ship nothing; the project's own manifest is
                # the satellite package above.
                continue
            version = described.get("versionInfo", "NOASSERTION")
            license_id = described.get("licenseConcluded") or described.get("licenseDeclared") or "NOASSERTION"
            doc.add(name, version, license_id, described.get("downloadLocation", "NOASSERTION"),
                    purl=f"pkg:vcpkg/{name}@{version.split('#')[0]}")
            found += 1
    if found == 0:
        sys.exit(f"write-sbom: no vcpkg.spdx.json under {vcpkg_installed}")
    pins = read("src/platform/windows/driver_pins.h")
    hm = define(pins, "SATELLITE_HIDMAESTRO_SDK_VERSION")
    vigem = define(pins, "SATELLITE_VIGEMBUS_BUNDLED_VERSION")
    doc.add("HIDMaestro", hm, "MIT", f"https://github.com/hifihedgehog/HIDMaestro/releases/tag/v{hm}",
            purl=f"pkg:github/hifihedgehog/HIDMaestro@v{hm}")
    doc.add("ViGEmBus", vigem, "BSD-3-Clause", f"https://github.com/nefarius/ViGEmBus/releases/tag/v{vigem}",
            purl=f"pkg:github/nefarius/ViGEmBus@v{vigem}")
    match = re.search(r"usbip-win2 (\d+(?:\.\d+)+)", read("redist/README.md"))
    if not match:
        sys.exit("write-sbom: redist/README.md names no usbip-win2 version")
    doc.add("usbip-win2", match.group(1), "BSD-2-Clause", "https://github.com/vadimgrn/usbip-win2",
            purl=f"pkg:github/vadimgrn/usbip-win2@v.{match.group(1)}")
    vigem_headers = re.search(r"## ViGEm Bus Driver SDK.*?Pinned-commit: (v[\d.]+)", read("lib/VENDORED.md"), re.S)
    doc.add("ViGEmClient", vigem_headers.group(1), "MIT", "https://github.com/ViGEm/ViGEmClient",
            purl=f"pkg:github/ViGEm/ViGEmClient@{vigem_headers.group(1)}")


def add_appimage(doc, appdir):
    libdir = os.path.join(appdir, "usr", "lib")
    libraries = sorted(f for f in os.listdir(libdir) if ".so" in f) if os.path.isdir(libdir) else []
    if not libraries:
        sys.exit(f"write-sbom: no shared libraries under {libdir}")
    owners = {}
    for library in libraries:
        result = subprocess.run(["dpkg-query", "-S", f"/{library}"], capture_output=True, text=True)
        lines = [line for line in result.stdout.splitlines() if ":" in line]
        candidates = [line.split(":")[0] for line in lines if line.rstrip().endswith("/" + library)]
        if not candidates:
            result = subprocess.run(["dpkg-query", "-S", library], capture_output=True, text=True)
            candidates = [line.split(":")[0] for line in result.stdout.splitlines() if line.rstrip().endswith("/" + library)]
        if not candidates:
            sys.exit(f"write-sbom: no Ubuntu package owns {library}")
        owners.setdefault(candidates[0], []).append(library)
    for package in sorted(owners):
        version = subprocess.run(["dpkg-query", "-W", "-f=${Version}", package],
                                 capture_output=True, text=True, check=True).stdout.strip()
        source = subprocess.run(["dpkg-query", "-W", "-f=${source:Package}", package],
                                capture_output=True, text=True, check=True).stdout.strip() or package
        doc.add(package, version, "NOASSERTION", f"https://packages.ubuntu.com/jammy/{package}",
                purl=f"pkg:deb/ubuntu/{package}@{version}?arch=amd64&distro=ubuntu-22.04&upstream={source}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lane", required=True, choices=["windows", "appimage", "deb", "rpm", "macos"])
    parser.add_argument("--out", required=True)
    parser.add_argument("--vcpkg-installed")
    parser.add_argument("--triplet", default="x64-windows-static")
    parser.add_argument("--appdir")
    parser.add_argument("--sentry", action="store_true")
    args = parser.parse_args()

    doc = Document(f"satellite-{args.lane}-components")
    doc.add("satellite", satellite_version(), "LGPL-3.0-or-later", "https://github.com/TinkerNorth/satellite",
            purl=f"pkg:github/TinkerNorth/satellite@{satellite_version()}", supplier="Organization: TinkerNorth")
    add_vendored(doc)
    # The Windows build links sentry-native (and its Crashpad) from vcpkg
    # whether or not it carries a DSN, and vcpkg's own document names it.
    if args.sentry and args.lane != "windows":
        add_sentry(doc)
    if args.lane == "windows":
        if not args.vcpkg_installed:
            sys.exit("write-sbom: --lane windows needs --vcpkg-installed")
        add_windows(doc, args.vcpkg_installed, args.triplet)
    elif args.lane == "appimage":
        if not args.appdir:
            sys.exit("write-sbom: --lane appimage needs --appdir")
        add_appimage(doc, args.appdir)
    doc.write(args.out)
    print(f"write-sbom: {args.out} names {len(doc.packages)} packages")


if __name__ == "__main__":
    main()
