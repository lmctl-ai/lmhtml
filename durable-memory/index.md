# lmhtml durable memory

`lmhtml` is an lmscript `_dsl_` host embedding Lexbor for HTML5 parsing, DOM manipulation, CSS selector queries, structural diffing, and templating. Linux release binaries dynamically link only system glibc (minimum 2.35); application libraries and C++ runtimes are static.
Repository: https://github.com/lmctl-ai/lmhtml.

## 60-second takeover

```sh
scripts/sync-deps.sh                  # Sync pinned Lexbor library
cmake -S . -B build                   # Configure CMake build
cmake --build build --parallel 4      # Build binary at build/lmhtml
./test.sh                             # Run full test suite (unit tests, ASan/UBSan, scripts)
```

## Document map

| File | Contents |
| --- | --- |
| [01-purpose-and-place.md](01-purpose-and-place.md) | Architectural role, Lexbor integration, and host-vs-script scope |
| [02-lexbor-static.md](02-lexbor-static.md) | Measured static build spike, archive sizes, and `lexbor.lock` pinning |
| [03-schema-v0.md](03-schema-v0.md) | DSL schema specification, host members, and selector methods |
| [04-algorithms.md](04-algorithms.md) | Normalization rules, Merkle subtree hashing, structural LCS diff |
| [05-plan.md](05-plan.md) | Milestones M1–M5, fixture testing strategy, and design risks |
| [06-open-items.md](06-open-items.md) | DSL ergonomics observations, set intersection, and host items |
| [07-build-run-test.md](07-build-run-test.md) | Static binary footprint, test pipeline results, and error handling |
| [08-m2-classes-ids.md](08-m2-classes-ids.md) | Class/ID harvesting, string similarity suggestions, and ASan checks |
| [09-m3-m4-diff-template.md](09-m3-m4-diff-template.md) | Structural diffing, tree template extraction, depth guards |
| [10-m5-rewrite.md](10-m5-rewrite.md) | In-place DOM rewriting, atomic saving, and mutation operations |
| [11-dogfood.md](11-dogfood.md) | Real-world static site dogfooding (Sphinx, Jekyll, mdBook) |
| [12-ci.md](12-ci.md) | GitHub Actions 5-platform matrix and compiler setup |
| [15-sdk-catalog.md](15-sdk-catalog.md) | SDK string and collection catalog integration, glob matching, and `sdk.lock` |

## SDK pin and sync

The vendored SDK headers and standard catalogs are pinned in `sdk.lock` pointing to `https://github.com/lmctl-ai/lmcode-cpp`:
```sh
scripts/sync-sdk.sh                   # Refetches pinned headers and verifies sha256
```

## Adding an example or function

### Adding an example
1. Create `examples/<name>.lms` using `_dsl_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest"`.
2. Run script: `build/lmhtml run examples/<name>.lms` or via `lmscript`.
3. Add the example to `./test.sh` test run list.
4. Add any visual/terminal output to `docs/images/<name>.png` and update `README.md` demo table with `lmctl autorun` command.

### Adding a host function
1. Declare function/method in `schema/lmhtml.dsl.json`.
2. Register and implement the function in C++ source under `src/` using `dslvm::Registry`.
3. Rebuild and verify schema synchronization with `cmake --build build` and `./test.sh`.

## Releases

Releases are triggered automatically on push of a `v*` tag. The release workflow builds across Linux x64/arm64, macOS x64/arm64, and Windows x64, publishing platform binaries with `SHA256SUMS`, `LICENSE`, and `THIRD_PARTY_NOTICES`.
