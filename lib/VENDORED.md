# Vendored third-party components

Source-of-truth inventory for components vendored under
`satellite/lib/` and `satellite/vigem/include/`. Ecosystem scanners
(OSV-Scanner, Dependabot, Trivy) cannot deduce these by reading the
source tree, so we maintain this file by hand and feed it to OSV-Scanner
through `satellite/osv-scanner.toml`.

Each component MUST list:

- **Component**: human-readable name
- **Upstream**: canonical repo URL
- **Pinned-commit**: 40-char SHA or release tag we vendored from
- **Last-vendored**: YYYY-MM-DD (must be within 90 days; CI fails otherwise)
- **License**: SPDX identifier
- **Notes**: any local modifications

If you bump a component, update its block here in the same PR. The
`vendored-freshness` job in `.github/workflows/security.yml` enforces
the 90-day freshness window.

---

## cpp-httplib (`lib/httplib.h`)

- Component: yhirose/cpp-httplib
- Upstream: https://github.com/yhirose/cpp-httplib
- Pinned-commit: 0.15.3
- Last-vendored: 2026-04-25
- License: MIT
- Notes: vendored as a single header. No local modifications. Used by
  `src/net/webserver.cpp` only.

## nlohmann/json (`lib/nlohmann/json.hpp`)

- Component: nlohmann/json
- Upstream: https://github.com/nlohmann/json
- Pinned-commit: v3.11.3
- Last-vendored: 2026-06-19
- License: MIT
- Notes: vendored as the single-header amalgamation. No local modifications.
  Used project-wide via `src/core/json.h` for response building and request /
  config / GitHub-API parsing.

## libsodium (`lib/libsodium/`)

- Component: jedisct1/libsodium
- Upstream: https://github.com/jedisct1/libsodium
- Pinned-commit: 1.0.20
- Last-vendored: 2026-04-25
- License: ISC
- Notes: bundled MinGW prebuilt archives (`libsodium-mingw.tar.gz`,
  `libsodium-win32/`, `libsodium-win64/`) consumed by the Windows
  toolchain. Linux + macOS link against the system package.

## ViGEm Bus Driver SDK (`vigem/include/ViGEm/`)

- Component: nefarius/ViGEmBus
- Upstream: https://github.com/nefarius/ViGEmBus
- Pinned-commit: setup-1.22.0
- Last-vendored: 2026-04-25
- License: BSD-3-Clause
- Notes: only the public C headers (`Common.h`, `BusShared.h`) are
  vendored. The runtime driver itself is installed by ViGEmBus's own
  installer on the user's machine. We do not ship driver code.
