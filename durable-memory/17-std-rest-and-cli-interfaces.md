# Standard `lmcode` REST Endpoints & Stateless CLI Interfaces

## Summary

`lmhtml` syncs its vendored `lmcode-cpp` SDK (`cpp/dslvm.hpp`, `cpp/stdlib/rest.hpp`, `cpp/stdlib/std.hpp`, `cpp/std/lmcode-std.schema.json`, `scripts/merge-std.py`, `sdk.lock`) to the 226-signature `lmcode_std_v3` standard catalog and `@lmctl-ai/lmctl@0.2.482`:
- Standard REST and stateless CLI interfaces (`RestGet`, `RestPost`, `RestPut`, `RestDelete`, `RestPatch`, `RestHead`, `RestOptions`, `RestHandler`, `CliCommand`, `CliOption`) and `RestExchange` / `rest_config` / `rest_route` / `rest_serve` / `rest_stop` / `rest_cli` are registered by `dslvm::Registry` via `cpp/stdlib/std.hpp`.
- `test.sh` verifies full and `--without-std` signature parity against the 226-signature standard catalog and runs an end-to-end `std-rest-cli` check exercising `CliOption`, `CliCommand`, `RestGet`, `rest_config`, and `rest_cli` with `html.parse(...)` and CSS selector queries.
