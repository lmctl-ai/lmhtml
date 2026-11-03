# Invocation envelope and run_lmcode (2026-11-02)

The current compiler (lmscript 0.2.454+) has no `savedsl`, `_dsl_`, `dsl_cli` or
CLI `.dslb` emission. A script runs a block with `r = run_lmcode(p)`: lmscript
compiles the block into private program files, starts `lmhtml --lmcode FILE...`
and reads exactly one terminal frame back. Programs added to one
`lmcode_build(...)` builder run in one host process, in add order.

## Host side (cpp/main.cpp)

- SDK synced to lmcode-cpp `7cf0916` (`include/dslvm.hpp` vendored as
  `cpp/dslvm.hpp`, plus `stdlib/invocation.hpp` and `stdlib/result_contract.hpp`);
  `sdk.lock` pins every file.
- `--lmcode` builds one `dslvm::InvocationResults::from_command(files)`. Every
  file loads before any program runs (a bad later file still prevents all writes),
  then each slot is selected, run and completed in order; the first failure stops
  the run. `write_frame` prints the single envelope.
- Exit codes: 0 success, 2 cli (usage, invalid metadata; a failure frame is
  printed and usage still goes to stderr), 3 load/run, 4 application
  (`Report.fail_if_any` raises `APP_FAIL`, recorded with phase `app`) or output failure.
- Dry mode is invocation-wide and comes from the `LMCODE_INVOCATION_FILE`
  metadata that `run_lmcode` writes (`lmscript --dry-run` sets `dry:true`); the
  envelope then ends with `"dry":true`. The standalone `--dry-run` flag requests
  the same mode when no metadata is present and is refused (cli) if metadata says
  real mode. The VM Context receives this dry mode. Explicit passive input/DOM
  reads run; mutation and writer callbacks without a preview are skipped.
  Bulk setters preserve the concrete node, and bulk getters passively copy it.
  Neither product files nor report files are written by preview execution.

## Tests (test.sh)

Fixtures no longer compile to `.dslb`. `compile_chunk` stores a program body and
`lmscript --check`s it; `run_prog [--dry-run] NAME...` writes a driver that runs
the stored bodies through `run_lmcode` (one program) or `lmcode_build` (several,
one host process) with an explicit command `sh hostwrap.sh TAG --lmcode $DSLFILE`.
`hostwrap.sh` records the host's own exit code (the test asserts it exactly), keeps
a copy of each private program file (used by the corrupt-bytecode and
load-before-run tests, which then call the host directly) and applies
`LMH_TIMEOUT`, `LMH_ULIMIT_V` and `LMH_ULIMIT_F` to the host only (a 1 GB address
limit would kill node).

Dropped: the "costly str_match refused fast" check. lmcode-cpp `8cca927` removed
the standard `str_match` 2^27 work budget by contract ("no hidden work or
allocation budgets"); the pattern now completes (about 110 s). lmhtml's own
`load_dir` pattern matching uses the same function, so it is unbounded too.
