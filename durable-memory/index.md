# lmhtml durable memory

Design and investigation notes for the lmhtml host (lexbor + cpp-dsl-sdk,
scripted by lmscript `_dsl_` bytecode). Read in numeric order.

- `01-purpose-and-place.md` — what lmhtml is; relation to lexbor,
  cpp-dsl-sdk, lmscript, lmcad/lmor-dsl; host-vs-script scope boundary.
- `02-lexbor-static.md` — measured static-build spike (12.3 s build,
  4.2 MiB archive, 2.0 MiB fully-static spike binary, `ldd` clean);
  module needs; C++ wrapping quirks; consume via pinned FetchContent-style
  `lexbor.lock`, mirroring `sdk.lock`.
- `03-schema-v0.md` — schema v0 JSON (`"dsl.schema": 2`, 35 host members),
  validated for real against the current compiler; use-case coverage map;
  deliberate omissions.
- `04-algorithms.md` — normalization rules, Merkle subtree hashing,
  LCS-based structural diff, common-subtree extraction and slot-based
  template emission; v0 out-of-scope list.
- `05-plan.md` — milestones M1–M5, fixture-based test strategy, foreseen
  DSL enhancements (listed, not implemented), risks.
- `06-open-items.md` — DSL friction found writing M1 scripts (no string
  predicates, O(n²) collection intersection, positional findings), plus
  host-side open items.
- `07-build-run-test.md` — measured M1 build/test numbers: 3,975,600-byte
  fully static binary, 23-check test.sh (release + ASan/UBSan), bugs the
  pipeline caught.
