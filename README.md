# lmhtml

A single self-contained native CLI for bulk HTML work (broken links, CSS
class/id typos, structural diff, shared-part extraction and rewrite), built
on lexbor and scriptable with lmscript `_dsl_` bytecode via the vendored
cpp-dsl-sdk header. See `durable-memory/` for design notes.

## Requirements

- C++17 toolchain, CMake >= 3.24, Ninja
- Node with `--experimental-strip-types` plus the lmscript sources at
  `~/lmctl-mono/lmscript` (override with `LMSCRIPT_SOURCE`), or an `lmscript`
  on PATH that emits DSL bytecode v5
- Python 3 (test assertions only)

## Setup

```sh
scripts/sync-sdk.sh      # vendor cpp/dslvm.hpp per sdk.lock
scripts/sync-lexbor.sh   # clone lexbor into third_party/ per lexbor.lock
```

## Build and run

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2

scripts/select-compiler.sh examples/check-links.lms  # resolves the compiler
scripts/lmscript-mono.sh examples/check-links.lms build/check-links.dslb
build/lmhtml --dsl build/check-links.dslb
```

CLI contract:

- `lmhtml --schema` — compact human/LLM summary of contexts, functions, types
- `lmhtml --schema --json` — the embedded schema bytes (compiler input)
- `lmhtml --dsl-signatures` — canonical registry signatures (parity checks)
- `lmhtml --dsl A.dslb [B.dslb ...]` — run bytecode files in order against the
  one embedded schema; all arguments are validated before anything runs;
  shared state across files; stops at the first failure

Exit codes: 0 ok, 2 usage/compile, 3 host load/run error, 4 application
failure. The default build links fully static (`-static`); disable with
`-DLMHTML_FULL_STATIC=OFF`.

## Test

```sh
./test.sh                            # release build + full pipeline
BUILD_DIR=build-sanitize ./test.sh   # ASan/UBSan variant
```
