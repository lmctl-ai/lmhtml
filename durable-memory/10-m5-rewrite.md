# M5: shared-part rewrite + safe save

Machine checked (2026-10-19), same box. Measured.

## What landed

- `Node.set_attr` / `remove` / `replace_with(html)` / `insert_before(html)` /
  `insert_after(html)` (schema +2 members → 47 signatures). Fragments parse
  via `lxb_html_document_parse_fragment` in the parent element's context.
  Gotcha: lexbor's `lxb_dom_node_insert_*` does NOT detach the node from its
  current parent — remove first or the splice loop never terminates.
- `Html.save(doc, path)`: atomic (temp file + rename in the same directory),
  output root = the process working directory (canonicalized; anything
  outside refused with exit 3), parent directories created. Inputs are only
  modified when the script explicitly saves to the same path.
  Write path (review fix): below the root every directory is opened with
  `O_NOFOLLOW` relative to its parent (created if missing), the temp file is
  a fresh `O_EXCL` name in the target directory (`.NAME.lmhtml-PID-N-RAND`),
  written with a checked loop, `fsync`'ed, given the replaced file's mode
  (new files: 0666 & ~umask), then `renameat`'ed; any failure unlinks it.
  The first version used a fixed `NAME.tmp-lmhtml` opened with ofstream: a
  planted symlink at that name redirected the write outside the root, the
  mode of replaced files was lost, a short write left the temp behind, and
  `save(d, "dir/")` created `dir` before failing. Owner/group and ACLs of a
  replaced file are not preserved (the new inode belongs to the runner).
- `--dry-run` CLI flag: `Html.save` writes nothing and reports a
  `would-save` finding (`N bytes, new|changed|unchanged`). Typical flow:
  dry-run, inspect findings, run for real; a dry-run right after a real run
  reports `unchanged` (idempotency probe).
- `examples/rewrite-nav.lms`: canonical nav + footer spliced into all 5
  fixture pages (2 drifted), saved to `build/rewritten/`, then M3 re-diff
  proves convergence (only `main` text differs: 2 findings per pair).
  Second run is byte-identical.

## What is lossy (serializer normalization, lexbor-inherent)

- Doctype re-emitted as `<!DOCTYPE html>` (case normalized, kept).
- Attribute values always double-quoted; entities re-escaped minimally
  (`&`, `"`, `<`); original entity spelling lost.
- Input encodings: lexbor parses to its internal UTF-8; output is UTF-8
  regardless of the source encoding.
- Inter-element whitespace survives as text nodes; nothing is pretty-printed
  or re-indented.

## Tests

`./test.sh` → **PASS: 43 checks** release / 42 ASan-UBSan. New M5 checks:
rewrite golden report, doctype kept in saved pages, idempotent rerun
(byte-identical), output-root escape refused with nothing written, dry-run
writes nothing and reports new/unchanged, empty replacement fragment
refused. Binary: **4,136,792 bytes**, fully static.
