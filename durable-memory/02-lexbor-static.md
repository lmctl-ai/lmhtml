# Lexbor static spike

Machine checked (2026-10-18), aarch64 Linux, 4 cores, build nice'd (`nice -n 19`).
All numbers below are **measured**, not estimated.

## What was done (reproduce from a scratch dir)

```sh
git clone --depth 1 https://github.com/lexbor/lexbor lexbor-spike   # HEAD f4cbbcd9
cd lexbor-spike
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLEXBOR_BUILD_STATIC=ON -DLEXBOR_BUILD_SHARED=OFF \
      -DLEXBOR_BUILD_TESTS=OFF -DLEXBOR_BUILD_EXAMPLES=OFF
nice -n 19 ninja -C build
# 40-line C++ spike (parse file, CSS-select, read href attr, serialize subtree):
g++ -O2 -std=c++17 -I source spike.cpp build/liblexbor_static.a -static -o spike
```

## Measured numbers

| fact | value |
|---|---|
| lexbor static build (cmake + ninja, 214 targets) | **12.3 s wall** |
| `liblexbor_static.a` (all modules, one archive) | **4,399,760 B (~4.2 MiB)** |
| spike binary, `g++ -O2 -static` (incl. static libstdc++/libc) | **2,128,888 B (~2.0 MiB)** |
| spike binary, stripped | **1,800,176 B (~1.7 MiB)** |
| `ldd spike` | `not a dynamic executable` — fully static works |
| license | Apache-2.0 (LICENSE in repo root); compatible, keep NOTICE-less attribution |

The spike ran correctly: `spike page.html 'a[href]'` matched 4 anchors,
printed `href` attributes and outer-HTML serializations; `:has()` and
attribute selectors with case flags work (per lexbor's own example).

## Modules

Default build produces **one monolithic archive** containing all modules:
core, css, dom, encoding, html, ns, punycode, selectors, style, tag, unicode,
url (+ ports, utils). `-DLEXBOR_BUILD_SEPARATELY=ON` splits per module.

lmhtml needs: **core, html, dom, css, selectors** (parse + query), **encoding,
ns, tag, unicode** (pulled in by html/css), and **url** (lexbor has a WHATWG
URL module — use it for resolving links relative to a page instead of
hand-rolling). `style` (computed styles) and `punycode` are not needed for v0
but ride along in the monolithic archive at zero cost. Decision: use the
monolithic static archive; do not bother with `LEXBOR_BUILD_SEPARATELY`.

## CSS stylesheet parsing (for the class-typo use case)

`lxb_css_stylesheet_parse()` (`source/lexbor/css/stylesheet.h:82`) parses a
full stylesheet; rules expose their selector lists, and
`lxb_css_selector_t` chains (`css/selectors/`) can be walked to harvest
`.class` / `#id` names. Selector matching uses
`lxb_css_selectors_parse()` + `lxb_selectors_find()` (verified in spike).

## API quirks for the C++ wrapper

- **Ownership**: every `lxb_*_create()` pairs with `lxb_*_destroy(obj, true)`.
  Nodes belong to their document — never free nodes; destroying the document
  frees the whole tree. Wrap document in a RAII holder; hand out raw node
  pointers into the arena (same pattern as lmcad/lmor-dsl handles).
- **Strings**: lexbor returns `(const lxb_char_t *, size_t)` pairs, not NUL
  terminated — always carry the length. Wrapper converts to `std::string`
  at the DSL boundary.
- **Errors**: `lxb_status_t` codes, mostly `LXB_STATUS_OK` or not; the HTML
  parser is error-tolerant by spec (a broken page still yields a tree), so
  "parse error" is rarely fatal — report, don't refuse.
- **Encoding**: parser auto-detects/sniffs; DOM text is UTF-8 internally.
  v0 treats everything as UTF-8 and says so in the report.
- **Serialization**: `lxb_html_serialize_cb(node, cb, ctx)` gives outer HTML
  of any subtree via callback (verified); pretty-print variant exists.
- C headers compile cleanly as C++; no namespace, all symbols `lxb_`-prefixed.

## How lmhtml should consume lexbor

Options: git submodule vs CMake FetchContent pinned by hash vs vendored copy.

**Recommendation: FetchContent pinned by commit hash**, mirroring how
`sdk.lock` pins lmcode-cpp:

- `lexbor.lock` (key=value, like sdk.lock) records repo + commit + the
  shallow-clone date; `scripts/sync-lexbor.sh` clones/fetches into
  `third_party/lexbor` (git-ignored, not committed) and checks out the pin.
- CMake `add_subdirectory(third_party/lexbor)` with
  `LEXBOR_BUILD_STATIC=ON SHARED=OFF TESTS=OFF EXAMPLES=OFF`, link
  `lexbor_static`, always `-static-libstdc++ -static-libgcc` plus an
  `LMHTML_FULL_STATIC` option adding `-static` (lmcad's pattern).
- Why not a submodule: sibling checkouts here are synced by lock scripts, not
  submodules; consistency beats novelty. Why not vendored: 4.2 MiB of upstream
  C is noise in diffs; lexbor is not header-only so the sdk.lock single-file
  trick does not apply. The pin hash in git gives reproducibility.
