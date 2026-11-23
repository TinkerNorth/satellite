## What changes, and why

<!-- Two or three sentences. Name the behaviour a user or a developer sees, not the files. -->

## Kind of change

- [ ] Fix
- [ ] Feature
- [ ] Refactor (behaviour unchanged)
- [ ] Docs, CI or repo plumbing
- [ ] Wire protocol or pairing flow (`docs/contract.md` changes; the five Dish clients need the matching change in the same release cycle)

## Shape of the code (CONTRIBUTING, "Shape of the code")

- [ ] Immutable by default; one flow per function; named steps instead of comments.
- [ ] Every `switch` over an enum is exhaustive, with no `default`.
- [ ] No singleton, no mutable static, no discarded result, no warning or lint suppression.
- [ ] `src/core/` stays platform-free (`scripts/check_core_purity.sh`); sockets, files and OS calls stay in `adapters/` or `platform/<os>/`.

## Tests

- [ ] Red first: a test failed before the change and passes after it.
- [ ] Every new flow has a test (`ctest`; the suites include `test_session_service`, `test_linux_platform`, `test_windows_platform`).
- [ ] A new dashboard string exists in all six `web/lang/*.json` locales.
- [ ] `scripts/ci-local.sh` (or `scripts\ci-local.ps1`) passes, with the `--with-` lanes the change touches.
- [ ] Manual check on Windows / macOS / Linux: <!-- which, with or without ViGEmBus or HIDMaestro -->
- [ ] Tested against a paired Dish client end to end: <!-- which client and version -->

## Bookkeeping

- [ ] `CHANGELOG.md` has a line under `[Unreleased]`.
- [ ] `tests/PARITY.md` is updated if a platform row changed.
- [ ] `docs/ARCHITECTURE.md`, `docs/contract.md` or `DESIGN.md` is updated if a rule, the wire or a token changed.
- [ ] `THIRD_PARTY.md` and `lib/VENDORED.md` are updated if a third-party component moved.
