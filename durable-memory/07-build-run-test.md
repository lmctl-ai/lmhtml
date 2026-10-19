# Build, run, test (M1)

Machine checked (2026-10-18/19), aarch64 Linux, 4 cores, builds nice'd
(`nice -n 19`, `-j2`). Measured, not estimated.

## Toolchain

- cpp-dsl-sdk vendored header pinned by `sdk.lock` at commit `62655a1d`
  (bytecode v5); `scripts/sync-sdk.sh --verify` is the first test check.
- lexbor pinned by `lexbor.lock` at `f4cbbcd9`, shallow-fetched into
  git-ignored `third_party/lexbor` by `scripts/sync-lexbor.sh`
  (`--verify` is the second test check).
- Compiler: `scripts/select-compiler.sh` probes a real chunk and requires
  `DSLB\x00\x05`; requires a compatible compiler on `PATH` or an explicit `LMSCRIPT_BIN`. The optional
  source adapter requires an explicit `LMSCRIPT_SOURCE` path and accepts version 5 only.

## Build

- Clean Release build (cmake + ninja, includes lexbor): **~17 s wall**.
- Incremental relink after one host file: ~1–7 s.
- Binary: **3,975,600 bytes**, `-static` by default (`LMHTML_FULL_STATIC=ON`);
  `ldd` reports `not a dynamic executable`. Asserted in test.sh.
- ASan/UBSan variant: `BUILD_DIR=build-sanitize ./test.sh` (static disabled;
  sanitizers and `-static` do not mix).

## Tests

`./test.sh` → **PASS: 56 checks** (release and sanitize alike):

1. sdk.lock + lexbor.lock pin verification.
2. check-links.lms compiles as bytecode v5 via the probed compiler.
3. `--schema --json` byte-identical to `schema/lmhtml.dsl.json`;
   `--schema` summary mechanically lists all 37 registry signatures;
   `--dsl-signatures` parity diff compiler vs host.
4. Fixture site link check: exactly 4 broken references (missing page,
   same-page anchor, cross-page anchor, `../` escape), text and JSON reports
   pinned byte-for-byte; rerun byte-identical (determinism).
5. Multi-file `--dsl` ordered execution, shared state, stop-on-first-failure
   (marker file must not appear), upfront argument validation (exit 2 before
   anything runs).
6. Refusals with exit-code pins: usage (2), `--schema FILE` (2), invalid
   selector (3), `at()` out of range (3), missing HTML file (3), stubbed
   later-milestone member (3), missing bytecode (3), schema-hash mismatch (3).
7. Fully static check on the shipped binary.

## Bugs the pipeline caught

- `lxb_dom_node_name` returns uppercase for HTML elements (DOM spec);
  `tag_name` now lowercases — without it `Doc.links()` found nothing.
- `lxb_css_selector_list_destroy_memory()` is mandatory: the parsed selector
  list owns a separate memory pool; ASan caught ~110 KB leaked per `select()`.
- `ldd` exits non-zero on static binaries; combined with `pipefail` this
  failed the static check even when the binary was correct.
