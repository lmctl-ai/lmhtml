# Algorithms: diff, common parts, templates

All of this lives **host-side** in C++ over lexbor DOM trees. The DSL only
chooses inputs, thresholds, and where results go.

## Normalization rules (shared by diff and hashing)

A node's *normalized form* is computed bottom-up:

- **Whitespace**: text nodes stripped of leading/trailing whitespace runs;
  internal runs collapse to one space; all-whitespace text nodes dropped —
  except inside `<pre>`/`<textarea>`/`<listing>`/`<plaintext>`, where text is
  kept verbatim. Each text run is keyed by its position among the element
  children (`a<b>x</b>b` ≠ `ab<b>x</b>`). Whitespace *between* inline
  elements is dropped (`<b>a</b> <i>b</i>` = `<b>a</b><i>b</i>`): accepted.
- **Attributes**: sorted by name; values verbatim; `class` attribute value
  split and sorted (class order is not semantic); boolean attributes
  (`disabled`, `checked`, ...) compare by presence only.
- **Ignored entirely**: comments, `<!DOCTYPE>`, `data-*` attributes (v0
  choice — they are page-instance noise more often than structure).
- **Kept**: tag name, attribute names/values, text content, child order.

Rationale: diff/compare must answer "same page structure?" not "same bytes?".

## Subtree hashing (`Node.hash`, common-part detection)

Merkle-style: `h(node) = H(tag, sorted attrs, normalized text, h(child1),
..., h(childN))` with H = FNV-1a 64 over a length-prefixed serialization
(domain-separated field tags). O(n) per tree, one pass, hashes cached per
node. Collision risk at site scale (<10^6 subtrees) is ~2^-32 birthday —
acceptable for a findings tool. FNV-1a is not collision resistant, so
`html.diff` treats a hash match only as a candidate and confirms it with an
exact normalized comparison before skipping a subtree.

## Structural diff (`html.diff(a, b)`)

Choice: **ordered-child recursive tree diff** (Zhang-Shasha-style edit
distance is O(n1*n2*depth) — overkill for same-site pages; simple recursive
matching is O(n) with a worst-case O(n*m) blowup only at sibling level).

Algorithm:
1. Walk both trees in lockstep from the roots.
2. At each element pair: equal normalized hashes → subtree equal, stop.
3. Otherwise compare tag/attrs/text → `changed` finding; then match children
   by **LCS over child hashes** (dynamic programming on the hash sequence,
   O(k1*k2) in sibling counts, small in practice) → matched pairs recurse,
   unmatched left = `removed`, unmatched right = `added`. Common prefix and
   suffix are trimmed first; if the rest still needs more than 4 Mi LCS cells
   (e.g. 2k x 2k changed siblings) a greedy in-order hash match is used
   instead (bounded memory; big reorders may show extra add/remove pairs).
   Unmatched leftovers with equal tags pair up and recurse; identical
   leftovers are moves and show as `removed` + `added`. Paths for `added`
   use the right-hand document's positions.
4. Each finding: `{kind: added|removed|changed, file, line: 0, message}` with
   a CSS-ish path (`body > div:nth(2) > p`) for location.

Deliberately not: cross-page node moves (reported as remove+add), attribute
edit distance, text diff within a node (whole-text changed/unchanged only).

## Common-part extraction (`html.write_template(docs, k, path)`)

Goal: find subtrees present (normalized-equal) in ≥ k of n pages; emit one
template with slots for the varying parts.

1. Hash every subtree of every page (cached; total O(sum of sizes)).
2. Bucket hashes → `pages(hash)` set. A hash is **common** when
   `|pages(hash)| >= k`.
3. Keep only **maximal** common subtrees (drop a common hash whose parent is
   also common in the same pages) — otherwise `<html>`/`<body>` swamp the
   report and nested commons duplicate.
4. Filter trivia: text-only or empty subtrees, and subtrees below a minimum
   size (≥ 2 elements or ≥ 40 normalized bytes — v0 constants, tune later).
5. Template emission: take the page with the most common content as the
   *skeleton*. Walk it top-down: where a child position varies across pages
   (hash not common at that slot), emit `<!-- slot:NAME -->` markers with a
   generated name (`slot-tag-N`); where common, emit the common markup once.
   Non-structural per-page variation inside a common subtree (e.g. a `class`
   differing on one page) splits that subtree out of "common" by
   construction — v0 accepts this strictness.
6. Output: template file + findings (one per common part, with page coverage
   `m of n`). Return value = number of slots.

Emission details (review fixes): the skeleton's `<!DOCTYPE>` is kept;
non-blank text directly inside a varying-but-recursed element becomes a
`<!-- slot:text-N -->` (never the skeleton page's own words); attributes of
recursed elements (e.g. `<body class="page-x">`) still come from the
skeleton page. Coverage counts a hash for a page only after an exact
normalized comparison with the first subtree seen for that hash.

Threshold semantics: `k` counts **pages containing** the subtree (a subtree
appearing twice in one page still counts once for coverage, but slots handle
repetition per position). `1 <= k <= pages` or the call is refused.

## Class/id typo support (mechanism, not policy)

- `classes_used()`: union of `class` attribute tokens over the document.
- `css_classes()`: `.class` names harvested by walking
  `lxb_css_selector_t` chains of every rule in every inline `<style>`
  stylesheet (`lxb_css_stylesheet_parse`).
- `ids()` / `#id` in selectors: same mechanics.
- `suggest(name, candidates)`: Damerau–Levenshtein (OSA variant), distance
  ≤ max(1, len/4) → matches sorted by distance. Pure string host function;
  the script decides used-vs-defined set differences by nested `len()` loops
  (small sets; quadratic is fine at page scale).

## Out of scope for v0

- Tree edit distance with moves; patch scripts (diff is read-only findings).
- Similarity/fuzzy common parts (near-equal subtrees); only exact-after-
  normalization matches.
- External stylesheets, `<link>`ed CSS, http(s) anything.
- Source line numbers (lexbor does not track them; findings use `line: 0`).
- Encoding beyond UTF-8; JS-generated DOM; shadow DOM.
