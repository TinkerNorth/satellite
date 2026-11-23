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

SENTRY_CLI_VERSION="3.8.2"
SENTRY_CLI_SHA256_LINUX_X86_64="c958865f9d4000183509ea21b1f04925d5390de6d8d87386b9e69f2b305070c3"
SENTRY_CLI_SHA256_WINDOWS_X86_64="50009f23c49d6e9ff7624204c7035855303b49c5e326708ee783c1fdf783ac83"

# The newest numbered linuxdeploy release; `continuous` is rebuilt in place,
# so it cannot be pinned by digest.
LINUXDEPLOY_VERSION="1-alpha-20251107-1"
LINUXDEPLOY_SHA256_X86_64="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"

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
