# Open items

## DSL friction found while writing M1 scripts

1. **No string predicates.** The link-check script cannot tell an anchor from
   a page link, so every broken reference is reported with the same kind:

   ```
   if (!html.link_exists(page, url)) {
     report.add("broken-link", page.path(), 0, url + " not found");
   }
   ```

   Nicer: `if (url.starts_with("#")) { report.add("broken-anchor", ...) }`.
   A minimal `string` predicate set (starts_with / ends_with / contains) —
   or host functions like `html.link_kind(url)` — would fix classification.
   This is the top ask so far.

2. **Intersecting two host collections is O(n²) from script.** For M2 class
   typos the script must loop `classes_used()` × `css_classes()` by hand
   (no lists/maps, R31). Works at page scale, but a host-side
   `difference(a, b)` / membership test would express the policy directly.

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

6. `Html.save` + mutation methods are registered stubs until M5 — scripts
   hitting them get exit 3 "not implemented yet (milestone M5)".
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
