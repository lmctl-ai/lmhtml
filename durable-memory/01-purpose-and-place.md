# Purpose and place

## Purpose

**lmhtml is a single self-contained native CLI for bulk HTML work**: checking
broken links, catching CSS class/id typos, structural diff of pages, extracting
common parts into a template, and rewriting shared parts across many pages.
It is scriptable: a user writes lmscript _lmcode_ blocks and the host executes
the bytecode and owns all state. Tool users declare cli="lmhtml" and
install="github:lmctl-ai/lmhtml@latest"; lmscript obtains the schema from the
installed host. Schema-file input remains useful for SDK authors and
compatibility tests.

## Place in the stack

- **lexbor** (C, Apache-2.0) supplies the HTML5 parser, DOM, CSS parser and
  selectors. lmhtml wraps it in C++; lexbor has no knowledge of the DSL.
- **lmcode-cpp** (`include/dslvm.hpp`, header-only, bytecode v5) is vendored as
  a single header `cpp/dslvm.hpp`, pinned by `sdk.lock` (repo/commit/sha256),
  exactly like lmcad. It executes `_lmcode_` programs; it never parses HTML.
- **lmscript**, published via npm as `@lmctl-ai/lmctl`, compiles `_lmcode_` blocks
  for the installed host and runs them with `run_lmcode` (see 16-invocation-envelope.md). Tool users need no schema file. Tests use the published compiler selected by scripts/select-compiler.sh; local compiler development may set LMSCRIPT_SOURCE explicitly.
- **lmcad / lmor-dsl** are the sibling hosts whose conventions lmhtml copies:
  one binary, schema embedded at compile time, `--lmcode` runs the program files `run_lmcode` writes and answers with one envelope,
  `--lmcode-schema` prints the embedded schema, `--lmcode-signatures` for parity checks,
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
