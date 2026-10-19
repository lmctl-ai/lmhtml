# Purpose and place

## Purpose

**lmhtml is a single self-contained native CLI for bulk HTML work**: checking
broken links, catching CSS class/id typos, structural diff of pages, extracting
common parts into a template, and rewriting shared parts across many pages.
It is scriptable: the operator writes lmscript `_dsl_` chunks against the one
embedded schema; the host executes the bytecode and owns all state.

## Place in the stack

- **lexbor** (C, Apache-2.0) supplies the HTML5 parser, DOM, CSS parser and
  selectors. lmhtml wraps it in C++; lexbor has no knowledge of the DSL.
- **cpp-dsl-sdk** (`include/dslvm.hpp`, header-only, bytecode v5) is vendored as
  a single header `cpp/dslvm.hpp`, pinned by `sdk.lock` (repo/commit/sha256),
  exactly like lmcad. It executes `_dsl_` bytecode; it never parses HTML.
- **lmscript** (in `~/lmctl-mono`, never modified) compiles `_dsl_` chunks
  against `schema/lmhtml.dsl.json` into `.dslb` bytecode. Tests compile through
  a thin Node `--experimental-strip-types` adapter importing `dsl.ts`, the same
  approach as lmcad's `scripts/lmscript-mono.{sh,mjs}`.
- **lmcad / lmor-dsl** are the sibling hosts whose conventions lmhtml copies:
  one binary, schema embedded at compile time, `--dsl` runs bytecode files,
  `--schema` prints the embedded schema, `--dsl-signatures` for parity checks,
  arena-owned host objects, exit codes 2/3/4.

## Scope boundary

- The **script expresses policy**: which pages, which selectors, thresholds,
  what to replace. Control flow only; no lists/maps in the language.
- The **host owns lifecycle and heavy algorithms**: parsing, serialization,
  link resolution, structural diff, common-subtree detection, template
  emission, report writing.
- **Not a browser.** No JS execution, no rendering, no fetching in v0
  (http(s) link checking is a separated optional capability, see 05-plan.md).
- Early development: schema v0 will break; breaking changes are fine.
