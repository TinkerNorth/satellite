#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# The pinned tools the gates and the release run with, each verified by
# SHA-256 before it runs. Sourced by scripts/ci-local.sh, by every workflow
# step that needs one of them, and by scripts/sentry-cli.sh and
# scripts/build-appimage.sh. A bump is a
# deliberate commit that changes the version and its digests here, and, for
# clang-format, reformats the tree in its own commit. The version-consistency
# workflow (scripts/check-version-consistency.sh) holds THIRD_PARTY.md and
# CONTRIBUTING.md to the versions written here.
#
#   source scripts/tools.sh
#   fetch_clang_format     # puts the pinned clang-format first on PATH
#   fetch_clang_tidy       # puts the pinned clang-tidy first on PATH
#   fetch_sentry_cli       # prints the path of the pinned sentry-cli
#   fetch_linuxdeploy      # prints the path of the pinned linuxdeploy AppImage

CLANG_FORMAT_VERSION="23.1.5"
# The PyPI wheels clang-format publishes, by platform tag. pip installs with
# --require-hashes, so a wheel whose digest is not listed here is refused.
CLANG_FORMAT_SHA256_MACOS_ARM64="3fb740335d939dd02252cd5075050fc8858466b16cf9c1efa2f52da3baa816a7"
CLANG_FORMAT_SHA256_MACOS_X86_64="32ec7498f9f1672dff2e65624cf317a1fd64e48a4f582d6c172d440c511bff00"
CLANG_FORMAT_SHA256_LINUX_X86_64="d97ce49911ba8c89ef3f3c0735a9c7818e4780ab4812c67d1c5fcd829d16ae52"
CLANG_FORMAT_SHA256_LINUX_AARCH64="e8037e12eae19a9779e01ca6719b8d7f6a939ade8869b061912551c9748a27ad"
CLANG_FORMAT_SHA256_WINDOWS_AMD64="18a4a1a698dd52ed4e288c97d99e93910f8f9d0128b266f65021ecf39c85d034"

# clang-tidy, the fleet's pin (dish-windows' scripts/tools.ps1 carries the
# same version), installed the way clang-format is: PyPI wheels, hash-checked.
CLANG_TIDY_VERSION="23.1.4"
CLANG_TIDY_SHA256_MACOS_ARM64="08e977575e9f800c1c806f2001a3cdfd01323491aec05e86dd24be9a5c2585f4"
CLANG_TIDY_SHA256_LINUX_X86_64="6bb31e4f319ecbe14acb75f23c3050d0f1d4b1d92ea1c66091bc76e4eac2c1ac"
CLANG_TIDY_SHA256_LINUX_AARCH64="87596cd77dedfdea23ced1d13e57e869dcaee99745bfe942796196da3c197671"
CLANG_TIDY_SHA256_WINDOWS_AMD64="8107e05930f53dc477dbb5fb8108d7c10464176fc95d3e58f4ee7317887aed59"

SENTRY_CLI_VERSION="3.8.2"
SENTRY_CLI_SHA256_LINUX_X86_64="c958865f9d4000183509ea21b1f04925d5390de6d8d87386b9e69f2b305070c3"
SENTRY_CLI_SHA256_WINDOWS_X86_64="50009f23c49d6e9ff7624204c7035855303b49c5e326708ee783c1fdf783ac83"

# The newest numbered linuxdeploy release; `continuous` is rebuilt in place,
# so it cannot be pinned by digest.
LINUXDEPLOY_VERSION="1-alpha-20251107-1"
LINUXDEPLOY_SHA256_X86_64="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"

# The security gates' tools (MIGRATION.md's table, each SHA-256 from the
# upstream checksum file), for scripts/ci-local.sh. CI's _security.yml pins
# the same versions and linux-amd64 digests.
OSV_SCANNER_VERSION="2.7.1"
OSV_SCANNER_SHA256_LINUX_AMD64="fb7dab19a8bd9bbe5c3189f09d4b7ee1820c9553a23bac202648a56efd2f6ee8"
OSV_SCANNER_SHA256_DARWIN_ARM64="b22c02877a811c8f78956e8643efa554932b7378f590d13f8e97d4e9448a29f3"
GITLEAKS_VERSION="8.30.1"
GITLEAKS_SHA256_LINUX_X64="551f6fc83ea457d62a0d98237cbad105af8d557003051f41f3e7ca7b3f2470eb"
GITLEAKS_SHA256_DARWIN_ARM64="b40ab0ae55c505963e365f271a8d3846efbc170aa17f2607f13df610a9aeb6a5"
PYYAML_VERSION="6.0.4"
# PyPI's digests of the CPython 3.12 to 3.15 wheels for macOS arm64, Linux
# x86_64 and Windows amd64.
PYYAML_SHA256S="4d30c2f186612bd2f9f07c08a4c96721af59cfc8da3ec7cb27dfa2834e8bb019 93bfb2459195406a518e2f487a9bbf01df240175f31e06f5a252ac6734c7bbf0 131d11bd9b60d705a6388366d77ed67850995a4d2be85fe149785a257c17e531 9d4211f080920629a90c9e31c51e326fb70cfae38f18d4128b9913c23642bace 50588147ab2c6a59b5f990a4905db80899a6c2bd197e046a6201f1e67587e794 0a3a351f04dc124f9357512bdb80ac37cd1750cf1371da33e7cbb96d80ac865f 7335809813ec838d9cd334b1744cda33deb141c9ef23a9c0df371c37980d3851 c604dc2613c417130479cdd6e55c2faa691b6365aa82b3dafddfcecc2eaf1ac7 1d2f08c122727c86f8303bb1dd45159ca1de1f2d68798e671ec46974e321008e 070c22b6f490d6cc30912be9fb9ab1af9c752769b39b53616f6510c3f038ee6a c57e40928738c4a202852759f9e1ee2c7063429a9469ee1ac15651bbf7d69963 67da066ca12eb60114ea87f2ecbcca622fff0c205c743cc900437127564fabf6"

TOOLS="${TOOLS:-${RUNNER_TEMP:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/.build}/tools}"
# Git for Windows' bash: RUNNER_TEMP arrives as D:\a\_temp; work in the POSIX
# form so every path below means the same thing to bash and to the tools.
if command -v cygpath >/dev/null 2>&1; then
    TOOLS="$(cygpath -u "$TOOLS")"
fi

tools_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

# A venv holding exactly the pinned wheel, installed with --require-hashes.
# The venv is named for the pin, so a bump builds a new one; its bin
# directory goes first on PATH for the caller's process.
fetch_clang_format() {
    local venv="$TOOLS/clang-format-$CLANG_FORMAT_VERSION"
    local bin="$venv/bin"
    local python="python3"
    case "$(uname -s)" in
        MINGW* | MSYS* | CYGWIN*)
            bin="$venv/Scripts"
            python="python"
            ;;
    esac
    if [ ! -x "$bin/clang-format" ] && [ ! -x "$bin/clang-format.exe" ]; then
        local requirements="$TOOLS/clang-format-requirements.txt"
        mkdir -p "$TOOLS"
        {
            printf 'clang-format==%s' "$CLANG_FORMAT_VERSION"
            local digest
            for digest in "$CLANG_FORMAT_SHA256_MACOS_ARM64" "$CLANG_FORMAT_SHA256_MACOS_X86_64" \
                "$CLANG_FORMAT_SHA256_LINUX_X86_64" "$CLANG_FORMAT_SHA256_LINUX_AARCH64" \
                "$CLANG_FORMAT_SHA256_WINDOWS_AMD64"; do
                printf ' --hash=sha256:%s' "$digest"
            done
            printf '\n'
        } >"$requirements"
        "$python" -m venv "$venv"
        "$bin/python" -m pip install --quiet --disable-pip-version-check \
            --only-binary=:all: --require-hashes -r "$requirements"
    fi
    export PATH="$bin:$PATH"
}

# Downloads the pinned sentry-cli for this platform into $TOOLS, checks its
# digest, and prints its path.
fetch_sentry_cli() {
    local name digest
    case "$(uname -s)" in
        Linux)
            name="sentry-cli-Linux-x86_64"
            digest="$SENTRY_CLI_SHA256_LINUX_X86_64"
            ;;
        MINGW* | MSYS* | CYGWIN*)
            name="sentry-cli-Windows-x86_64.exe"
            digest="$SENTRY_CLI_SHA256_WINDOWS_X86_64"
            ;;
        *)
            echo "fetch_sentry_cli: no pinned sentry-cli for $(uname -s)" >&2
            return 1
            ;;
    esac
    local cli="$TOOLS/sentry-cli-$SENTRY_CLI_VERSION/$name"
    if [ ! -x "$cli" ] || [ "$(tools_sha256 "$cli")" != "$digest" ]; then
        mkdir -p "$(dirname "$cli")"
        curl -fsSL --retry 3 -o "$cli" \
            "https://github.com/getsentry/sentry-cli/releases/download/$SENTRY_CLI_VERSION/$name"
        local got
        got="$(tools_sha256 "$cli")"
        if [ "$got" != "$digest" ]; then
            rm -f "$cli"
            echo "fetch_sentry_cli: $name has SHA-256 $got, expected $digest" >&2
            return 1
        fi
        chmod +x "$cli"
    fi
    echo "$cli"
}

# Downloads the pinned linuxdeploy AppImage into $TOOLS, checks its digest,
# and prints its path.
fetch_linuxdeploy() {
    local tool="$TOOLS/linuxdeploy-$LINUXDEPLOY_VERSION/linuxdeploy-x86_64.AppImage"
    if [ ! -x "$tool" ] || [ "$(tools_sha256 "$tool")" != "$LINUXDEPLOY_SHA256_X86_64" ]; then
        mkdir -p "$(dirname "$tool")"
        curl -fsSL --retry 3 -o "$tool" \
            "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_VERSION/linuxdeploy-x86_64.AppImage"
        local got
        got="$(tools_sha256 "$tool")"
        if [ "$got" != "$LINUXDEPLOY_SHA256_X86_64" ]; then
            rm -f "$tool"
            echo "fetch_linuxdeploy: linuxdeploy has SHA-256 $got, expected $LINUXDEPLOY_SHA256_X86_64" >&2
            return 1
        fi
        chmod +x "$tool"
    fi
    echo "$tool"
}

# tools_fetch_checked <url> <sha256> <path>: download once, check the digest.
tools_fetch_checked() {
    local url="$1" digest="$2" dest="$3"
    if [ -f "$dest" ] && [ "$(tools_sha256 "$dest")" = "$digest" ]; then
        return 0
    fi
    mkdir -p "$(dirname "$dest")"
    curl -fsSL --retry 3 -o "$dest" "$url"
    local got
    got="$(tools_sha256 "$dest")"
    if [ "$got" != "$digest" ]; then
        rm -f "$dest"
        echo "tools.sh: $(basename "$dest") has SHA-256 $got, expected $digest" >&2
        return 1
    fi
}

tools_platform() {
    case "$(uname -s)-$(uname -m)" in
        Linux-x86_64) echo linux-amd64 ;;
        Darwin-arm64) echo darwin-arm64 ;;
        *)
            echo "tools.sh: no pinned security tool for $(uname -s)-$(uname -m)" >&2
            return 1
            ;;
    esac
}

# Prints the path of the pinned OSV-Scanner.
fetch_osv_scanner() {
    local platform name digest
    platform="$(tools_platform)" || return 1
    case "$platform" in
        linux-amd64) name="osv-scanner_linux_amd64" digest="$OSV_SCANNER_SHA256_LINUX_AMD64" ;;
        darwin-arm64) name="osv-scanner_darwin_arm64" digest="$OSV_SCANNER_SHA256_DARWIN_ARM64" ;;
    esac
    local bin="$TOOLS/osv-scanner-$OSV_SCANNER_VERSION/$name"
    tools_fetch_checked "https://github.com/google/osv-scanner/releases/download/v$OSV_SCANNER_VERSION/$name" \
        "$digest" "$bin" || return 1
    chmod +x "$bin"
    echo "$bin"
}

# Prints the path of the pinned gitleaks.
fetch_gitleaks() {
    local platform asset digest
    platform="$(tools_platform)" || return 1
    case "$platform" in
        linux-amd64) asset="gitleaks_${GITLEAKS_VERSION}_linux_x64.tar.gz" digest="$GITLEAKS_SHA256_LINUX_X64" ;;
        darwin-arm64) asset="gitleaks_${GITLEAKS_VERSION}_darwin_arm64.tar.gz" digest="$GITLEAKS_SHA256_DARWIN_ARM64" ;;
    esac
    local dir="$TOOLS/gitleaks-$GITLEAKS_VERSION"
    tools_fetch_checked "https://github.com/gitleaks/gitleaks/releases/download/v$GITLEAKS_VERSION/$asset" \
        "$digest" "$dir/$asset" || return 1
    tar -xzf "$dir/$asset" -C "$dir" gitleaks
    echo "$dir/gitleaks"
}

# A venv holding the pinned PyYAML (for scripts/security-allowlist.py);
# prints its python.
fetch_pyyaml_python() {
    local venv="$TOOLS/pyyaml-$PYYAML_VERSION"
    if [ ! -x "$venv/bin/python" ]; then
        local requirements="$TOOLS/pyyaml-requirements.txt"
        mkdir -p "$TOOLS"
        {
            printf 'PyYAML==%s' "$PYYAML_VERSION"
            local digest
            for digest in $PYYAML_SHA256S; do printf ' --hash=sha256:%s' "$digest"; done
            printf '\n'
        } >"$requirements"
        python3 -m venv "$venv"
        "$venv/bin/python" -m pip install --quiet --disable-pip-version-check \
            --only-binary=:all: --require-hashes -r "$requirements" >&2
    fi
    echo "$venv/bin/python"
}

# A venv holding exactly the pinned clang-tidy wheel; its bin directory goes
# first on PATH for the caller's process.
fetch_clang_tidy() {
    local venv="$TOOLS/clang-tidy-$CLANG_TIDY_VERSION"
    local bin="$venv/bin"
    local python="python3"
    case "$(uname -s)" in
        MINGW* | MSYS* | CYGWIN*)
            bin="$venv/Scripts"
            python="python"
            ;;
    esac
    if [ ! -x "$bin/clang-tidy" ] && [ ! -x "$bin/clang-tidy.exe" ]; then
        local requirements="$TOOLS/clang-tidy-requirements.txt"
        mkdir -p "$TOOLS"
        {
            printf 'clang-tidy==%s' "$CLANG_TIDY_VERSION"
            local digest
            for digest in "$CLANG_TIDY_SHA256_MACOS_ARM64" "$CLANG_TIDY_SHA256_LINUX_X86_64" \
                "$CLANG_TIDY_SHA256_LINUX_AARCH64" "$CLANG_TIDY_SHA256_WINDOWS_AMD64"; do
                printf ' --hash=sha256:%s' "$digest"
            done
            printf '\n'
        } >"$requirements"
        "$python" -m venv "$venv"
        "$bin/python" -m pip install --quiet --disable-pip-version-check \
            --only-binary=:all: --require-hashes -r "$requirements"
    fi
    export PATH="$bin:$PATH"
}
