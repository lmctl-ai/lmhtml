# lmhtml

## Status

Experimental. The CLI, schema, and script API are evolving; bytecode v5 and schema v2 are required.

A single self-contained native CLI for bulk HTML work (broken links, CSS
class/id typos, structural diff, shared-part extraction and rewrite), built
on lexbor and scriptable with lmscript `_dsl_` bytecode via the vendored
cpp-dsl-sdk header. See `durable-memory/` for design notes.

## Requirements

- C++17 toolchain, CMake >= 3.24, Ninja
- An lmscript compiler that emits DSL bytecode v5: `npm i -g @lmctl-ai/lmctl`
  (Node.js 24.15 or newer) puts `lmscript` on PATH; `scripts/select-compiler.sh` probes it.
  Set `LMSCRIPT_BIN` to select an explicit compiler executable. For local
  compiler-source development, set `LMSCRIPT_SOURCE` and use
  `scripts/lmscript-source.sh` directly.
- Python 3 (test assertions only)

## Setup

```sh
scripts/sync-sdk.sh      # vendor cpp/dslvm.hpp per sdk.lock
scripts/sync-lexbor.sh   # clone lexbor into third_party/ per lexbor.lock
```

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2

scripts/select-compiler.sh  # prints the compatible compiler path
lmscript examples/check-links.lms build/check-links.dslb
build/lmhtml --dsl build/check-links.dslb
```

CLI contract:

- `lmhtml --schema` — compact human/LLM summary of contexts, functions, types
- `lmhtml --schema --json` — the embedded schema bytes (compiler input)
- `lmhtml --dsl-signatures` — canonical registry signatures (parity checks)
- `lmhtml [--dry-run] --dsl A.dslb [B.dslb ...]` — run bytecode files in order
  against the one embedded schema; all files are loaded and verified before
  anything runs; shared state across files; stops at the first failure.
  With `--dry-run`, `Html.save` reports a `would-save` finding
  (`N bytes, new|changed|unchanged`) and writes nothing.

Exit codes: 0 ok, 2 usage/compile, 3 host load/run error, 4 application
failure. The default build links fully static (`-static`); disable with
`-DLMHTML_FULL_STATIC=OFF`.

## Usage guide (the five use cases)

All work is an lmscript `_dsl_` chunk compiled against `schema/lmhtml.dsl.json`
(see `examples/` for complete runnable scripts):

1. **Broken links** — `examples/check-links.lms`: `html.load_dir(dir)` →
   `Doc.links()` (a/link/img/script/srcset) → policy via
   `html.link_kind(page, url)` (`internal-page`, `internal-asset`, `anchor`,
   `external`, `special`, `outside-root`) plus `str_match(url, glob)` for
   hosting-noise patterns → `html.link_exists` → `report.add` +
   `report.write_text/json`. `load_dir(dir, include, exclude)` filters pages
   by glob on the dir-relative path (`blog/2024/a.html`): `""` or `"*"`
   include everything, exclude wins over include, `*` crosses `/`, `**/`
   also matches zero directories, a leading `./` is ignored; absolute
   patterns must lie below `dir`, and patterns leaving it are refused.
   Hidden files and dirs are included; directory symlinks are not followed;
   results are sorted by path.
2. **CSS class/id typos** — `examples/class-typos.lms`: site-wide sets in one
   call (`DocList.classes_used()` / `css_classes()` / `ids()` / `css_ids()`,
   covering inline `<style>`, linked local stylesheets and `@media` blocks);
   `str_difference` + `str_union` + `str_list` for custom set flow;
   `suggest(name, candidates)` gives edit-distance near misses; ignore
   JS-hook patterns with `str_match(name, "language-*")`. Glob syntax:
   `*` any run (including `/`), `?` one byte, `[...]` class with ranges,
   `[!...]` negated class, `\x` literal x (an unterminated `[` is literal);
   bytes, not code points; worst case is refused beyond 2^27 steps.
3. **Structural diff** — `examples/diff-pages.lms`: `html.diff(a, b)` ignores
   whitespace/comment/attribute-order/`data-*` noise and emits
   `diff-added/removed/changed` findings with CSS-ish paths; `Node.hash()` is
   the normalized subtree fingerprint.
4. **Common parts → template** — `examples/make-template.lms`:
   `html.write_template(pages, k, out)` finds subtrees shared by ≥ k pages
   and emits a sample-based template with `<!-- slot:TAG-N -->` markers;
   `common-part` findings report page coverage.
5. **Shared-part rewrite** — `examples/rewrite-nav.lms`: `select` +
   `replace_with`/`set_attr`/`insert_before`/`insert_after`/`remove`, then
   `html.save(page, path)`. Saves are atomic (temp + rename), confined to the
   working directory (the output root), and never touch inputs unless the
   script explicitly saves to the same path. Re-diff with `html.diff` to
   verify convergence; re-runs are idempotent. Use `--dry-run` first.

Serialization is mostly faithful but normalizes (verified): doctype becomes
`<!DOCTYPE html>` and whitespace before `<html>` is dropped; missing
`<html>`/`<head>`/`<body>` wrappers and optional tags are added (`<tbody>`,
`</td>`, `</p>`, ...); tag names are lowercased; attribute values are always
double-quoted with `&`, `"`, `<`, `>` and U+00A0 escaped, and boolean
attributes gain `=""` (`hidden` → `hidden=""`); `<br/>` becomes `<br>`;
character references are decoded (`&copy;` is written as UTF-8 `©`).
Indentation between elements is kept as text. lexbor does not decode legacy
encodings, so `Html.save` refuses a page whose source is not valid UTF-8
rather than writing mixed Latin-1/UTF-8 bytes.

## Test

```sh
./test.sh                            # release build + full pipeline
BUILD_DIR=build-sanitize ./test.sh   # ASan/UBSan variant
```

## License

lmhtml's own code (including the vendored `cpp/dslvm.hpp` runtime) is MIT —
see `LICENSE`. Third-party components and their licenses are listed in
`THIRD_PARTY_NOTICES` (lexbor is Apache-2.0; runtime-library notes for the
static binaries are there too).

### Validation scripts

`Doc.byte_size()` returns the original UTF-8 source size in bytes.
`Doc.doctype()` returns the leading doctype name (empty when missing or preceded
by content; comments and whitespace are ignored). `Doc.markup_errors()` returns
an explicit-tag nesting audit from lexbor tokens **before** tree repair: unlike
HTML itself, this audit requires closing tags even where HTML allows omission.
These three values describe the loaded source, not subsequent DOM mutations.
`Node.attributes()` lists attribute names in document order; `Node.path()` gives
a CSS path using element sibling positions. Paths reflect the current tree.
After writing findings, call `Report.fail_if_any()` to return a nonzero host
exit with `APP_FAIL`; an empty report succeeds. Reports otherwise remain
informational, preserving existing analysis and rewrite scripts.
