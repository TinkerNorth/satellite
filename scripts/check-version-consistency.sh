#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Fails when a fact written in more than one place drifts. One script, run by
# version-consistency.yml on every pull request and push to main, by
# release.yml against the tag (--tag), and by scripts/ci-local.sh, so the
# workflow and a local run cannot disagree.
#
#   scripts/check-version-consistency.sh             every check below
#   scripts/check-version-consistency.sh --tag 1.2.3 also: the tag names
#                                                    /VERSION
#
# What it holds together:
#
#   The version. /VERSION is the source of truth. src/core/version.h mirrors
#   it for C++ and satellite.rc, vcpkg.json carries its own "version" (vcpkg
#   does not read /VERSION), the AppStream metainfo's newest <release> names
#   it, CHANGELOG.md has a heading for it, and installer.iss derives it at
#   preprocess time (the check holds the preprocessor line in place).
#
#   The bundled drivers. src/platform/windows/driver_pins.h is what the
#   dashboard's driver banner compares installed drivers against, and
#   installer.iss carries the same ViGEmBus / HIDMaestro versions for the
#   payload it ships; each driver carries a release version and the driver
#   binary's own, and both pairs are checked. The HIDMaestro SDK tag is also
#   written in scripts/redist-functions.ps1, redist/SHA256SUMS,
#   scripts/generate-sbom.ps1 and lib/VENDORED.md.
#
#   The pins repeated across files: vcpkg.json's builtin-baseline (and no
#   workflow falling back to the runner image's vcpkg), sentry-native in the
#   overlay port and the Linux FetchContent, the vendored headers against
#   lib/VENDORED.md, the Flatpak manifest's libsodium and opus against the
#   versions every other build ships, and every version THIRD_PARTY.md and
#   CONTRIBUTING.md state against the file that pins it.
#
#   The security and hardening tools the shared _security.yml and
#   _release_harden.yml pin, against scripts/tools.sh and THIRD_PARTY.md, and
#   the SLSA generator commit SECURITY.md's recipe names, against release.yml.
#
#   Facts documents repeat from code: the protocol number (src/core/types.h)
#   in docs/contract.md and SECURITY.md, and the C++ standard (CMakeLists.txt)
#   in CONTRIBUTING.md.
set -euo pipefail
cd "$(dirname "$0")/.."

tag=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --tag)
      shift
      tag="${1:?--tag needs a value}"
      ;;
    --tag=*) tag="${1#--tag=}" ;;
    *) echo "usage: scripts/check-version-consistency.sh [--tag X.Y.Z]" >&2; exit 2 ;;
  esac
  shift
done

fail=0
err() {
  echo "::error::$*"
  fail=1
}
# expect <what> <got> <want>
expect() {
  if [ -z "$2" ] || [ "$2" != "$3" ]; then
    err "$1 is '${2}', expected '${3}'"
  fi
}
# The first quoted value after the match, on the first line matching an
# extended regex.
quoted() {
  grep -E -m1 "$1" "$2" | sed -E "s/.*$1[^\"]*\"([^\"]*)\".*/\\1/" || true
}
# The Version column of THIRD_PARTY.md's summary row for a component.
third_party() {
  grep -E -m1 "^\| \[$1\]" THIRD_PARTY.md | awk -F'|' '{gsub(/^ +| +$/, "", $3); print $3}' || true
}

# ── The version ──────────────────────────────────────────────────────────
if [ ! -f VERSION ]; then
  echo "::error::/VERSION is missing; it's the authoritative version string."
  exit 1
fi
ver=$(tr -d '[:space:]' < VERSION)
if ! [[ "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "::error::VERSION content '$ver' is not in MAJOR.MINOR.PATCH form."
  exit 1
fi
IFS='.' read -r major minor patch <<<"$ver"
echo "VERSION = $ver"

define() {
  grep -E "^[[:space:]]*#define[[:space:]]+$1[[:space:]]" "$2" | head -1 | awk '{print $3}' | tr -d '"'
}
expect "src/core/version.h SATELLITE_VERSION_MAJOR" "$(define SATELLITE_VERSION_MAJOR src/core/version.h)" "$major"
expect "src/core/version.h SATELLITE_VERSION_MINOR" "$(define SATELLITE_VERSION_MINOR src/core/version.h)" "$minor"
expect "src/core/version.h SATELLITE_VERSION_PATCH" "$(define SATELLITE_VERSION_PATCH src/core/version.h)" "$patch"
expect "src/core/version.h SATELLITE_VERSION_STRING" "$(define SATELLITE_VERSION_STRING src/core/version.h)" "$ver"
expect "src/core/version.h SATELLITE_VERSION_COMMA" \
  "$(grep -E '^[[:space:]]*#define[[:space:]]+SATELLITE_VERSION_COMMA' src/core/version.h | sed -E 's/[^0-9,]//g')" "$major,$minor,$patch,0"
expect "src/core/version.h SATELLITE_VERSION_DOTTED" "$(define SATELLITE_VERSION_DOTTED src/core/version.h)" "$major.$minor.$patch.0"

if ! grep -q 'FileOpen(SourcePath + "VERSION")' installer.iss; then
  err "installer.iss is not reading /VERSION at preprocess time; see the #define MyAppVersion block."
fi
if grep -E '^#define[[:space:]]+MyAppVersion[[:space:]]+"[0-9]' installer.iss >/dev/null; then
  err "installer.iss has a hard-coded MyAppVersion literal; it derives from /VERSION."
fi

expect "vcpkg.json version" "$(quoted '"version"[[:space:]]*:' vcpkg.json)" "$ver"

metainfo=packaging/io.github.tinkernorth.satellite.metainfo.xml
expect "$metainfo newest <release>" "$(grep -m1 -oE '<release version="[^"]+"' "$metainfo" | sed -E 's/.*"([^"]+)"/\1/')" "$ver"

if ! grep -qE "^## \[?${ver//./\\.}\]?( |$)" CHANGELOG.md; then
  err "CHANGELOG.md has no '## ${ver}' heading"
fi

if [ -n "$tag" ]; then
  bare="${tag#v}"
  bare="${bare%%[-+]*}"
  expect "the tag ${tag} (without a prerelease suffix)" "$bare" "$ver"
fi

# ── Bundled drivers ──────────────────────────────────────────────────────
pins=src/platform/windows/driver_pins.h
expect "installer.iss ViGEmBusVersion" "$(define ViGEmBusVersion installer.iss)" "$(define SATELLITE_VIGEMBUS_BUNDLED_VERSION "$pins")"
expect "installer.iss ViGEmBusDriverVersion" "$(define ViGEmBusDriverVersion installer.iss)" "$(define SATELLITE_VIGEMBUS_BUNDLED_DRIVER_VERSION "$pins")"
hm="$(define SATELLITE_HIDMAESTRO_SDK_VERSION "$pins")"
expect "installer.iss HmVersion" "$(define HmVersion installer.iss)" "$hm"
for driver in SATELLITE_VIGEMBUS_BUNDLED_DRIVER_VERSION SATELLITE_HIDMAESTRO_BUNDLED_DRIVER_VERSION; do
  [[ "$(define "$driver" "$pins")" =~ ^[0-9]+(\.[0-9]+){1,3}$ ]] || err "$driver in $pins is not a dotted version"
done
[[ "$hm" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || err "SATELLITE_HIDMAESTRO_SDK_VERSION '$hm' is not MAJOR.MINOR.PATCH"
other_hm="$(grep -ohE 'HIDMaestro-v[0-9]+\.[0-9]+\.[0-9]+|HIDMaestro/v[0-9]+\.[0-9]+\.[0-9]+|download/v[0-9]+\.[0-9]+\.[0-9]+/HIDMaestro' \
  scripts/redist-functions.ps1 redist/SHA256SUMS | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | sort -u | tr '\n' ' ')"
expect "the HIDMaestro tags in scripts/redist-functions.ps1 and redist/SHA256SUMS" "${other_hm% }" "$hm"
expect "scripts/generate-sbom.ps1 HIDMaestro" "$(grep -oE 'hidmaestro@[0-9.]+' scripts/generate-sbom.ps1 | head -1 | cut -d@ -f2)" "$hm"
expect "lib/VENDORED.md HIDMaestro Pinned-commit" \
  "$(awk '/^## HIDMaestro/{f=1} f && /Pinned-commit:/{sub(/.*Pinned-commit: v?/, ""); print; exit}' lib/VENDORED.md)" "$hm"

# ── vcpkg ────────────────────────────────────────────────────────────────
baseline="$(quoted '"builtin-baseline"' vcpkg.json)"
[[ "$baseline" =~ ^[0-9a-f]{40}$ ]] || err "vcpkg.json has no 40-character builtin-baseline"
if grep -n 'VCPKG_INSTALLATION_ROOT' .github/workflows/*.yml; then
  err "a workflow uses the runner image's vcpkg (VCPKG_INSTALLATION_ROOT); run scripts/setup-vcpkg.ps1, which checks out the baseline"
fi

# ── sentry-native ────────────────────────────────────────────────────────
sentry_port="$(quoted '"version"[[:space:]]*:' vcpkg/ports/sentry-native/vcpkg.json)"
sentry_fetch="$(grep -oE 'sentry-native/releases/download/[0-9.]+/' CMakeLists.txt | head -1 | cut -d/ -f4)"
expect "CMakeLists.txt's sentry-native FetchContent" "$sentry_fetch" "$sentry_port"

# ── Vendored components against lib/VENDORED.md ──────────────────────────
vendored_pin() {
  awk -v h="^## $1" '$0 ~ h {f=1} f && /Pinned-commit:/{sub(/.*Pinned-commit: /, ""); sub(/ .*/, ""); print; exit}' lib/VENDORED.md
}
httplib="$(quoted '#define CPPHTTPLIB_VERSION ' lib/httplib.h)"
expect "lib/VENDORED.md cpp-httplib Pinned-commit" "$(vendored_pin cpp-httplib)" "v$httplib"
json="$(define NLOHMANN_JSON_VERSION_MAJOR lib/nlohmann/json.hpp).$(define NLOHMANN_JSON_VERSION_MINOR lib/nlohmann/json.hpp).$(define NLOHMANN_JSON_VERSION_PATCH lib/nlohmann/json.hpp)"
expect "lib/VENDORED.md nlohmann/json Pinned-commit" "$(vendored_pin nlohmann/json)" "v$json"
sodium="$(quoted '#define SODIUM_VERSION_STRING' lib/libsodium/libsodium-win64/include/sodium/version.h)"
expect "lib/VENDORED.md libsodium Pinned-commit" "$(vendored_pin libsodium)" "$sodium-RELEASE"

# ── Flatpak sources ──────────────────────────────────────────────────────
flatpak=packaging/flatpak/io.github.tinkernorth.satellite.yml
expect "$flatpak libsodium" "$(grep -oE 'libsodium-[0-9.]+\.tar\.gz' "$flatpak" | sed -E 's/libsodium-(.*)\.tar\.gz/\1/')" "$sodium"
opus="$(third_party opus)"
expect "$flatpak opus" "$(grep -oE 'opus-[0-9.]+\.tar\.gz' "$flatpak" | sed -E 's/opus-(.*)\.tar\.gz/\1/')" "$opus"

# ── THIRD_PARTY.md and CONTRIBUTING.md against the pins ──────────────────
if [ ! -f THIRD_PARTY.md ]; then
  err "THIRD_PARTY.md is missing"
else
  tools_pin() { grep -E "^$1=" scripts/tools.sh | cut -d'"' -f2; }
  expect "THIRD_PARTY.md cpp-httplib" "$(third_party cpp-httplib)" "$httplib"
  expect "THIRD_PARTY.md nlohmann/json" "$(third_party nlohmann/json)" "$json"
  expect "THIRD_PARTY.md libsodium" "$(third_party libsodium)" "$sodium"
  expect "THIRD_PARTY.md sentry-native" "$(third_party sentry-native)" "$sentry_port"
  expect "THIRD_PARTY.md HIDMaestro" "$(third_party HIDMaestro)" "$hm"
  expect "THIRD_PARTY.md ViGEmBus" "$(third_party ViGEmBus)" "$(define SATELLITE_VIGEMBUS_BUNDLED_VERSION "$pins")"
  expect "THIRD_PARTY.md vcpkg" "$(third_party vcpkg)" "$baseline"
  expect "THIRD_PARTY.md clang-format" "$(third_party clang-format)" "$(tools_pin CLANG_FORMAT_VERSION)"
  expect "THIRD_PARTY.md sentry-cli" "$(third_party sentry-cli)" "$(tools_pin SENTRY_CLI_VERSION)"
  expect "THIRD_PARTY.md linuxdeploy" "$(third_party linuxdeploy)" "$(tools_pin LINUXDEPLOY_VERSION)"
  expect "THIRD_PARTY.md Inno Setup" "$(third_party 'Inno Setup')" "$(grep -m1 -E '^\$version = ' scripts/install-inno-setup.ps1 | cut -d"'" -f2)"
fi
# The shared workflows pin the security and hardening tools; the local copies
# in scripts/tools.sh and the rows in THIRD_PARTY.md follow them.
shared_pin() { grep -m1 -E "^[[:space:]]+$1: \"" "$2" | cut -d'"' -f2; }
osv_pin="$(shared_pin OSV_SCANNER_VERSION .github/workflows/_security.yml)"
gitleaks_pin="$(shared_pin GITLEAKS_VERSION .github/workflows/_security.yml)"
expect "scripts/tools.sh OSV_SCANNER_VERSION" "$(grep -E '^OSV_SCANNER_VERSION=' scripts/tools.sh | cut -d'"' -f2)" "$osv_pin"
expect "scripts/tools.sh GITLEAKS_VERSION" "$(grep -E '^GITLEAKS_VERSION=' scripts/tools.sh | cut -d'"' -f2)" "$gitleaks_pin"
expect "scripts/tools.sh PYYAML_VERSION" "$(grep -E '^PYYAML_VERSION=' scripts/tools.sh | cut -d'"' -f2)" "$(shared_pin PYYAML_VERSION .github/workflows/_security.yml)"
if [ -f THIRD_PARTY.md ]; then
  expect "THIRD_PARTY.md OSV-Scanner" "$(third_party OSV-Scanner)" "$osv_pin"
  expect "THIRD_PARTY.md gitleaks" "$(third_party gitleaks)" "$gitleaks_pin"
  expect "THIRD_PARTY.md cosign" "$(third_party cosign)" "$(shared_pin COSIGN_VERSION .github/workflows/_release_harden.yml)"
  expect "THIRD_PARTY.md Syft" "$(third_party Syft)" "$(shared_pin SYFT_VERSION .github/workflows/_release_harden.yml)"
  expect "THIRD_PARTY.md Grype" "$(third_party Grype)" "$(shared_pin GRYPE_VERSION .github/workflows/_release_harden.yml)"
fi

# The SLSA generator commit SECURITY.md tells users to verify against is the
# one release.yml calls.
slsa_sha="$(grep -oE 'generator_generic_slsa3\.yml@[0-9a-f]{40}' .github/workflows/release.yml | head -1 | cut -d@ -f2)"
expect "SECURITY.md's SLSA generator commit" \
  "$(grep -oE 'generator_generic_slsa3\.yml@[0-9a-f]{40}' SECURITY.md | sort -u | cut -d@ -f2 | tr '\n' ' ' | sed 's/ $//')" "$slsa_sha"

expect "CONTRIBUTING.md's clang-format version" \
  "$(grep -oE 'clang-format \*\*[0-9.]+\*\*' CONTRIBUTING.md | head -1 | grep -oE '[0-9.]+')" "$(grep -E '^CLANG_FORMAT_VERSION=' scripts/tools.sh | cut -d'"' -f2)"

# ── Facts the documents repeat from code ─────────────────────────────────
protocol="$(grep -E 'inline const int PROTOCOL_VERSION = ' src/core/types.h | grep -oE '[0-9]+')"
expect "docs/contract.md's protocol version" "$(grep -m1 -oE '^Version: protocol [0-9]+' docs/contract.md | grep -oE '[0-9]+')" "$protocol"
expect "SECURITY.md's protocol number" "$(grep -m1 -oE 'The wire protocol \(protocol [0-9]+' SECURITY.md | grep -oE '[0-9]+$')" "$protocol"
cxx="$(grep -E '^set\(CMAKE_CXX_STANDARD [0-9]+\)' CMakeLists.txt | grep -oE '[0-9]+')"
named="$(grep -oE 'C\+\+[0-9]{2}' CONTRIBUTING.md | sort -u | tr '\n' ' ')"
expect "the C++ standard CONTRIBUTING.md names" "${named% }" "C++$cxx"

if [ "$fail" -ne 0 ]; then
  echo
  echo "::error::Pinned facts drifted. Fix the file each error names (the pin's own file is the source: /VERSION, driver_pins.h, scripts/tools.sh, vcpkg.json, lib/VENDORED.md, src/core/types.h, CMakeLists.txt)."
  exit 1
fi
echo "All pinned facts agree (version ${ver}, protocol ${protocol}, C++${cxx})."
