# Plan

## Milestones

Each milestone ends green: `test.sh` PASS, committed, pushed.

- **M1 — skeleton + link check** (end-to-end first):
  repo scaffolding mirroring lmcad (`scripts/sync-sdk.sh` + `sdk.lock`,
  `scripts/sync-lexbor.sh` + `lexbor.lock` per 02-lexbor-static.md,
  `scripts/lmscript-source.{sh,mjs}` adapter, CMake with embedded
  `schema/lmhtml.dsl.json`, `-static-libstdc++ -static-libgcc` +
  `LMHTML_FULL_STATIC`); C++ host with arena-owned `Doc`/`Node` handles and
  host-side argument validation (the VM trusts compiler-produced bytecode);
  schema subset: `load`/`load_dir`/`select`/`attr`/`links`/`resolve`/
  `link_exists`/`Report`; examples `check-links.lms`.
  Done = script → bytecode → host → text+JSON report on a fixture site with
  one deliberately broken relative link and one broken `#anchor`.
- **M2 — CSS class/id typos**: `classes_used`/`css_classes`/`ids`/`suggest`;
  inline `<style>` only; example `class-typos.lms` emitting
  used-but-undefined, defined-but-unused, near-miss suggestions.
- **M3 — structural diff**: normalization + hashing + `html.diff` +
  `Node.hash`; example `diff-pages.lms` on a fixture pair with known edits.
- **M4 — common parts + template**: `html.write_template`; fixture site of
  5 pages sharing header/footer/nav with 2 drifted; template + slot count
  asserted.
- **M5 — shared-part rewrite**: `replace_with`/`set_attr`/`remove`/`save`
  applied across `load_dir` pages; example `rewrite-nav.lms`; verify by
  re-diffing (M3) that all pages converge.

## Optional capability decision: http(s) link checking

**Out of v0..v1.** It needs a network stack (defeats "no extra .so, fully
static" unless we embed one), timeouts, robots/caching policy — a different
product surface. The schema stays compatible: `link_exists` on `http(s)`
URLs returns false-with-finding `kind=external-unchecked` in v0, so scripts
written today still work when/if an `html.check_remote(bool)` toggle arrives.

## Test strategy

Mirror lmcad: one `test.sh` (bash, `ok()/fail()` counters) + Python stdlib
heredocs, everything through the real compile→run pipeline.

1. `scripts/sync-sdk.sh --verify` first; lexbor pin check second.
2. Compile examples, assert `DSLB` magic + version byte 5.
3. `lmhtml --dsl-schema` byte-identical to `schema/lmhtml.dsl.json`;
   `--dsl-signatures` parity diff compiler vs host.
4. Fixtures under `tests/fixtures/`: `site/` (5 pages + shared header/nav,
   broken links, one class typo), `diff-a.html`/`diff-b.html` (known edits),
   `drift/` (pages with one divergent header). Fixtures are small,
   hand-written, checked in.
5. Golden-output pins: report text/JSON compared byte-for-byte; rewrite
   results compared against expected files.
6. Refusal cases with exit-code pins: usage → 2, schema-hash mismatch → 3,
   bad selector / missing file → 3 with message, bounds on `at()`.
7. Determinism: run link check + template twice, byte-identical outputs.
8. Sanitizer build `BUILD_DIR=build-sanitize ./test.sh` (ASan/UBSan) like
   lmcad — DOM pointer arenas are exactly where use-after-free hides.

## DSL enhancements foreseen (listed, NOT implemented)

- **String contains/starts_with/regex**: scripts currently can't filter
  links or class names by pattern; today they must push everything through
  host methods. Likely the first real ask (host functions can fill the gap).
- **Lists/maps in the language**: the language has no built-in list or map values; the `StrList` host
  object pattern works but `suggest` returning `StrList` then intersecting
  two host lists from script is clumsy — watch pain level in M2.
- **Line/column in DOM**: would need lexbor parse instrumentation; biggest
  report-quality gap (findings all say `line: 0`).
- **String interpolation `${}` inside DSL**: embedding is an lmscript
  pre-process step only; inside the chunk string building is `+` concat.
- **Parallel page processing**: the host runs one chunk; multi-file
  `--dsl a.dslb b.dslb` is sequential. Fine until fixture sites get large.

## Risks / open items

- lexbor DOM mutation API (`replace_with` via fragment parsing) is the least
  verified part — spike it first thing in M1, before building on it.
- `load_dir` ordering must be sorted for determinism (filesystem order is
  not stable).
- Schema will churn until M3; that's allowed (breaking changes fine) but
  recompile all fixtures on every schema edit — test.sh does this.
