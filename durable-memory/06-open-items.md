# Open items

## DSL friction found while writing M1 scripts

1. **No string predicates.** **Resolved in M2** (host-side): ordinary schema
   functions bound in `cpp/strlib.*` (kept separate so they can later move
   into a cpp-dsl-sdk catalog; other hosts would then drop theirs):
   `str_starts_with`, `str_ends_with`, `str_contains` (bool, string,string),
   `str_lower` (ASCII), `str_substr(s, start, len)` (byte-based, clamped),
   `str_len` (bytes).

2. **Intersecting two host collections is O(n²) from script.** **Resolved in
   M2**: `str_difference(a, b) -> StrList` is a host-side hash set
   (order-preserving, deduped); `suggest(name, candidates)` does
   Damerau-Levenshtein (OSA) host-side, threshold max(1, len/4), sorted by
   distance. Scripts express set policy in one call.

3. **`report.add(kind, file, line, message)` positional params.** Four
   same-ish strings in a row invite swaps; named/keyword arguments or a
   finding-builder would be safer. Tolerable for now.

4. **Findings carry `line: 0` forever** until someone instruments lexbor
   parse positions (see 03-schema-v0.md quirk). Byte offsets would already
   be an improvement; not scheduled.

5. **`${}` embedding works only outside the chunk** (lmscript pre-process).
   Inside `_dsl_`, string building is `+` concatenation — fine, but easy to
   forget when moving code between the two layers.

## Host / wiring open items

6. ~~`Html.save` + mutation methods are registered stubs~~ **Done in M5**:
   all schema members are implemented; the safety rules (atomic save, cwd
   output root, `--dry-run`) are in 10-m5-rewrite.md.
7. `link_exists` reuses pages already loaded (e.g. by `load_dir`) for
   cross-page anchors and parses other targets once per check (no cache).
   Link policy (shared by `Html.resolve`, see `resolve_link` in main.cpp):
   whitespace trimmed; schemes, `//host` and absolute `<base href>` are
   external (unchecked, count as existing); `?query` ignored; path and
   fragment percent-decoded; relative `<base href>` honoured; `/x` resolves
   against the `load_dir` directory (cwd for `load`/`parse`); `load_dir`
   pages may not escape their directory; `dir/` means `dir/index.html`;
   `#` and `#top` always exist. Paths are case-sensitive (like the server).
8. `load_dir` includes `.html`/`.htm` recursively, sorted by path for
   determinism. No ignore rules (e.g. skip `node_modules`-style dirs) yet.
9. lmcad's `tests/*.py` refusal suites are a good model; lmhtml currently
   keeps refusals inline in test.sh — split out when the list grows.
10. `css_classes`/`css_ids` harvest **top-level style rules of inline
    `<style>` only**: rules nested in `@media` blocks are skipped, external
    stylesheets are not read. Both need a decision (walk at-rule children;
    load linked CSS relative to the page).
11. lexbor cleanup gotchas found by ASan: `lxb_css_selector_list_destroy_memory`
    after each selector-list use (M1), and `lxb_css_parser_selectors_destroy`
    before `lxb_css_parser_destroy` when `lxb_css_parser_selectors_init` was
    called (M2). Any new CSS entry point must pair these.
