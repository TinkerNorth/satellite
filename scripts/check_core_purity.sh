#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Core purity gate (PLAN of record: docs/architecture.md, "pure core" split).
#
# Gate 1 — src/core/ may include ONLY:
#   * C/C++ standard headers (curated allowlist below),
#   * other src/core/ headers ("core/x.h" or the bare intra-core "x.h"),
#   * <nlohmann/json.hpp> — the sanctioned JSON facade (core/json.h wraps it).
#   Everything else fails: httplib, sodium/OpenSSL, sockets, <windows.h>,
#   <IOKit/...>, ViGEm, and any src/net | src/platform | src/app header. Core
#   must stay compilable on every platform with zero I/O/OS surface.
#
# Gate 2 — src/net/ and src/adapters/ must never name a per-OS platform path
#   (e.g. "platform/windows/crypto.h"). The sanctioned seam is the bare
#   mirrored header ("config.h", "crypto.h", "tray.h"): each platform provides
#   its own copy and CMake picks the directory. Naming an OS directly defeats
#   that seam and breaks the other two builds.
#
# Both gates scan *.h/*.hpp/*.cpp/*.mm and treat ObjC's `#import` exactly
# like `#include` — a .mm file cannot smuggle a header past the gate by
# spelling the directive differently.
#
# Plain bash 3.2 + grep/sed (BSD and GNU), no dependencies; runs locally and
# as a cheap CI step:  bash scripts/check_core_purity.sh
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0

# Curated C/C++ standard-header allowlist. Extend it deliberately (one header
# at a time, with review) — never with a wildcard: the narrowness IS the gate.
STD_HEADERS=" algorithm any array atomic bit bitset cctype cerrno cfloat
charconv chrono cinttypes climits cmath compare complex concepts
condition_variable csignal cstdarg cstddef cstdint cstdio cstdlib cstring
ctime cwchar deque exception filesystem format forward_list fstream
functional future initializer_list iomanip ios iosfwd iostream istream
iterator limits list locale map memory mutex new numbers numeric optional
ostream queue random ranges ratio regex set shared_mutex span sstream stack
stdexcept streambuf string string_view system_error thread tuple type_traits
typeindex typeinfo unordered_map unordered_set utility variant vector version
assert.h ctype.h errno.h float.h inttypes.h limits.h locale.h math.h signal.h
stdarg.h stddef.h stdint.h stdio.h stdlib.h string.h time.h wchar.h "
# Collapse the newlines above so the space-delimited membership test works.
STD_HEADERS=" $(echo $STD_HEADERS) "

# Anything matching these (case-insensitive) is named-and-shamed with a
# specific reason even though the allowlist would reject it anyway.
is_forbidden_reason() {
    local hdr="$1"
    case "$(printf '%s' "$hdr" | tr '[:upper:]' '[:lower:]')" in
        *httplib*)               echo "httplib must never be visible to core (webserver shell only)" ;;
        iokit/*|*corefoundation*) echo "IOKit/CoreFoundation are macOS platform surface" ;;
        windows.h|*winsock*|ws2tcpip.h|wincrypt.h|winhttp.h) echo "Win32 headers are platform surface" ;;
        *vigem*)                 echo "ViGEm belongs to the Windows backend" ;;
        sodium.h|*openssl*)      echo "crypto libraries stay out of core (net/session_crypto owns them)" ;;
        net/*|*/net/*)           echo "core must not depend on src/net (dependency points the other way)" ;;
        platform/*|*/platform/*) echo "core must not depend on src/platform" ;;
        app/*|*/app/*)           echo "core must not depend on src/app" ;;
        *)                       return 1 ;;
    esac
}

report() { # file lineno include reason
    printf '%s:%s: #include %s\n    -> %s\n' "$1" "$2" "$3" "$4"
    fail=1
}

# ---- Gate 1: src/core ------------------------------------------------------
for f in $(find src/core -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.mm' \) | sort); do
    while IFS= read -r hit; do
        [ -n "$hit" ] || continue
        lineno="${hit%%:*}"
        line="${hit#*:}"
        hdr="$(printf '%s' "$line" | sed -E 's/^[[:space:]]*#[[:space:]]*(include|import)[[:space:]]*[<"]([^">]+)[">].*/\2/')"

        if reason="$(is_forbidden_reason "$hdr")"; then
            report "$f" "$lineno" "$hdr" "$reason"
            continue
        fi

        case "$line" in
            *'<'*)
                # Angle include: the JSON facade or a std header, nothing else.
                if [ "$hdr" = "nlohmann/json.hpp" ]; then
                    continue
                fi
                case "$STD_HEADERS" in
                    *" $hdr "*) continue ;;
                esac
                report "$f" "$lineno" "<$hdr>" \
                    "not a C/C++ standard header (or extend the commented allowlist deliberately)"
                ;;
            *)
                # Quoted include: must resolve inside src/core itself.
                if [ -f "src/core/${hdr#core/}" ]; then
                    continue
                fi
                report "$f" "$lineno" "\"$hdr\"" \
                    "quoted includes in core must resolve to another src/core header"
                ;;
        esac
    done <<EOF
$(grep -nE '^[[:space:]]*#[[:space:]]*(include|import)' "$f" || true)
EOF
done

# ---- Gate 2: net/adapters must use the mirrored-header seam ----------------
for f in $(find src/net src/adapters -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.mm' \) | sort); do
    while IFS= read -r hit; do
        [ -n "$hit" ] || continue
        lineno="${hit%%:*}"
        line="${hit#*:}"
        hdr="$(printf '%s' "$line" | sed -E 's/^[[:space:]]*#[[:space:]]*(include|import)[[:space:]]*[<"]([^">]+)[">].*/\2/')"
        case "$hdr" in
            platform/*|*/platform/*)
                report "$f" "$lineno" "\"$hdr\"" \
                    "net/adapters must use the bare mirrored header (\"config.h\"/\"crypto.h\"/\"tray.h\"), never a platform/<os>/ path"
                ;;
        esac
    done <<EOF
$(grep -nE '^[[:space:]]*#[[:space:]]*(include|import)' "$f" || true)
EOF
done

if [ "$fail" -ne 0 ]; then
    echo
    echo "core purity gate: FAILED (see violations above)"
    exit 1
fi
echo "core purity gate: OK (src/core is std+intra-core+nlohmann only; net/adapters respect the platform seam)"
