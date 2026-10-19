# M2: CSS class/id typos

Machine checked (2026-10-19), same box as 07-build-run-test.md. Measured.

## What landed

- `Doc.classes_used` / `Doc.ids` walk `class`/`id` attributes; results sorted
  and deduped.
- `Doc.css_classes` / `Doc.css_ids` parse every inline `<style>` with
  `lxb_css_stylesheet_parse` and harvest `.class` / `#id` names by walking
  `lxb_css_selector_t` chains of top-level style rules (`@media` nesting and
  external sheets deferred — open item 10).
- `suggest(name, candidates)`: OSA edit distance, threshold max(1, len/4),
  sorted by distance then name. Caught `usde`→`used`, `teem`→`team` in the
  fixtures.
- `str_difference(a, b)`: host-side hash set — the script never loops two
  collections O(n²) (M1 friction item 2, resolved).
- `str_starts_with` / `str_ends_with` / `str_contains` / `str_lower` /
  `str_substr` / `str_len` in separated `cpp/strlib.*` (M1 friction item 1,
  resolved; catalog listed in 06-open-items.md).

Schema: 45 canonical signatures (37 in M1 + `Doc.css_ids` + 7 functions).
`examples/class-typos.lms` reports class-undefined (+suggestion),
class-unused, id-unused (+suggestion); findings pinned byte-for-byte.

## Tests

`./test.sh` → **PASS: 30 checks** (release), **PASS: 29** (ASan/UBSan
variant, static check skipped). New: class-typos golden text+JSON, str_*
behavior chunk, stub refusal moved to `Html.diff`. Binary now
**4,055,056 bytes**, still fully static.

## Bugs caught this milestone

- `lxb_css_parser_selectors_init` pairs with
  `lxb_css_parser_selectors_destroy` — ASan caught 288 B/call leaked without
  it (both cleanup paths fixed).
- Tag names from the DOM are uppercase (M1 fix); class names and ids are
  case-sensitive and compared verbatim — matches CSS/HTML semantics.
