# Dogfood on real sites

Machine checked (2026-10-19). Sites mirrored with wget into the scratch dir
`~/poc/lmhtml-real` (NOT committed; licenses fine for local testing):
**rust-book** (mdbook, 114 pages, 8.3 MB), **jekyll** (jekyllrb.com, 211
pages, 7.7 MB), **sphinx** (sphinx-doc.org, 203 pages, 12 MB). First mirrors
flattened directories / dropped `?query` assets — re-mirrored preserving
hierarchy and stripping query suffixes from filenames. Scripts in
`lmhtml-real/scripts/`, measured with `/usr/bin/time -v`.

## Numbers (wall time / peak RSS / findings)

| site | use case | time | RSS | findings |
|---|---|---|---|---|
| rust-book | links | 2.12 s | 63 MB | 66 broken |
| jekyll | links | 0.29 s | 85 MB | 1 broken |
| sphinx | links | 2.97 s | 189 MB | 218 broken |
| rust-book | typos | 0.28 s | 65 MB | 513 undef / 104 unused |
| jekyll | typos | 0.43 s | 90 MB | 685 undef / 90 unused |
| sphinx | typos | 0.85 s | 194 MB | 4683 undef / 69 unused |
| rust-book | diff (2 chapters) | 0.00 s | 4 MB | 54 |
| sphinx | diff (2 pages) | 0.00 s | 4 MB | 218 |
| rust-book | template k=100 | 0.59 s | 113 MB | 13 parts, 37 slots |
| jekyll | template k=190 | 0.64 s | 133 MB | 5 parts, 6 slots |
| rust-book | rewrite .menu-bar | 0.49 s | 65 MB | 113 pages |
| sphinx | rewrite div.sphinxsidebar | 0.88 s | 187 MB | 203 pages |
| jekyll | rewrite footer | 0.80 s | 90 MB | 208 pages |

Dry-run after a real rewrite: 113/113 `unchanged` (idempotency on real data).

## Finding classification (hand-sampled 20 each)

- **rust-book links**: 20/20 are `../reference/`, `../std/` escapes to sibling
  Rust docs not in the mirror — genuinely missing locally (TP for a
  containment check) but intended-external for the author (noise).
- **jekyll links**: 1 finding total — `docs/pages/team.md` linked from
  security page; the .md source was never built. **True positive, real bug.**
- **sphinx links**: 20/20 are `/_/static/...` and `/cdn-cgi/...` injected by
  the ReadTheDocs/Cloudflare hosting layer — not site content. FP for authors.
- **class-undefined** (all sites): 20/20 per site are JS-hook classes
  (`language-rust`, `ignore`, `edition2024`, `fa-*`, Rouge `language-*`,
  sphinx theme classes) — classes intentionally absent from CSS. FP for typo
  hunting; no real typos found on these mature docs (expected).
- **class-unused** (site-wide): 20/20 are JS-toggled/plugin classes
  (`algolia-*`, `ace_editor`, `ayu`, `active`) — correct as "not in static
  HTML", FP as "dead CSS".

Before fixes: typo check **crashed** (arena limit, then at-rule segfault) or
flooded per-page unused (37k lines on jekyll). After: bounded, site-wide,
readable.

## Host fixes made (with synthetic regression fixtures in tests/fixtures/realworld/)

1. **External stylesheets + @media** (open item 10 closed): css_classes/css_ids
   read `<link rel="stylesheet">` local files (link-resolution policy) and
   recurse @media blocks. Segfault found: the at-rule payload union differs
   per kind — only recurse when `at->type == LXB_CSS_AT_RULE_MEDIA`.
2. **srcset**: `links()` collects img/source srcset URLs (comma split with
   data:-URI tail rejoin).
3. **Collection arena limit 4096 → 65536**: one suggest() per undefined class
   per page blew 4096 on a 114-page site.
4. **str_union / str_list** schema functions: site-wide aggregation of
   used/defined classes (per-page "unused" was pure noise).

## DSL/schema enhancements real usage suggests

1. **Link kind classification**: script needs `out-of-root escape` vs
   `hosting-noise` vs `truly broken` — today only `resolve()` + str_*
   predicates. A `html.link_kind(url) -> string` would do.
2. **Glob/regex matching** (`str_match(s, glob)`) for ignore-lists:
   JS-hook class patterns (`language-*`, `fa-*`) should be filterable.
3. **Aggregation**: the str_list()+str_union accumulator pattern works but a
   host-side `DocList`-level `classes_used()` (site-wide in one call) would
   kill the boilerplate.
4. **common-part findings need a path/selector**, not just the tag name —
   five "script shared by..." lines are indistinguishable.
5. **load_dir include/exclude globs** (skip generated `_modules/` source
   dumps on sphinx — 100+ pages of noise).
6. **Per-page CSS caching**: same stylesheet re-parsed per page; fine at
   200 pages (0.9 s) but linear waste.
