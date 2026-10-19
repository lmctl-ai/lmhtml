# M3 + M4: structural diff, common parts, templates

Machine checked (2026-10-19), same box. Measured.

## Depth guard (Opus review fix, landed before M3)

Documents nested deeper than **512** elements are refused with a clean DSL
error naming the file (`nesting depth exceeds 512 in FILE`). Two layers: a
byte-level prescan before parsing (pathological nesting stalls the parser —
300k nested `<div>` took ~30 s), and an authoritative iterative DOM depth
check after. test.sh deletes its temp dir now (trap on EXIT).

## M3 — structural diff

- Normalization and FNV-1a Merkle hashing live in `cpp/analyze.*`, exactly
  per 04-algorithms.md (whitespace collapsed, comments/doctype dropped,
  attrs sorted, class tokens sorted, data-* dropped, child order kept).
- `Node.hash()` exposes the fingerprint; `Html.diff(a, b) -> int` appends
  `diff-added` / `diff-removed` / `diff-changed` findings with CSS-ish paths
  (`html > body:nth-child(2) > p.intro:nth-child(3)`) and returns the count.
- Children matched by LCS over hashes; equal-tag leftovers pair up and
  recurse (changed), the rest are added/removed.
- Fixture `tests/fixtures/diff/`: a vs b = 4 findings pinned byte-for-byte;
  a-noise (whitespace/comment/attr-order/data-*) diffs to **0**.

## M4 — common parts + template

- `Html.write_template(docs, k, path) -> int` (slots). A hash is common when
  it appears in ≥ k pages; trivial subtrees filtered (< 2 elements and
  < 40 normalized bytes); skeleton = page richest in substantial commons.
  Varying child positions become `<!-- slot:TAG-N -->`; subtrees that vary
  but contain shared parts are recursed, not slotted.
- Fixture `tests/fixtures/tpl/`: 5 pages, nav drifted in p5, footer drifted
  in p4 → k=4 finds head/header (5/5), nav/footer (4/5); emitted template
  and findings pinned byte-for-byte; slots = 1.

## Tests

`./test.sh` → **PASS: 35 checks** release / **34** ASan-UBSan (static check
skipped). New: diff golden, noise-invariance, hash stability, template
golden, k<1 refusal, depth guard (600 refused naming the file, 500 parses).
Binary: **4,128,816 bytes**, fully static.

## Bugs caught

- `lxb_html_serialize_cb` serializes only the node itself; the tree variant
  is `lxb_html_serialize_tree_cb`. `Node.outer_html` had been shallow since
  M1 — nothing pinned it until M4's template emission.
- `hash_all` refused the document node (not an element) and never recursed —
  every page hashed to nothing, template was all slots.
