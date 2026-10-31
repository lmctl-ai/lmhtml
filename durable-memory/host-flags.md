# Host flag cutover

Only --lmcode, --lmcode-schema and --lmcode-signatures select the host language
protocol. Retired options are usage exit 2 before input effects; new metadata and
execution keep existing error/result behavior. DSLB magic and .dslb paths remain.

The consumer SDK is refreshed through sync-sdk.sh at 20325f7. Compiler probes,
bytecode checks and standard signature counts now verify actual v6/206-member
capability instead of historical package-version strings. Original parser controls
fail for every retired mode; renamed controls pass before normal integration.

The full Linux gate passes 68 checks: native DOM/profile features, schema and
signatures, source ranges, fragments, controlled serialization, mutations and
atomic writes, report failures, intrinsic image dimensions and Markdown/plain
CLI/DSL integrations and determinism. Tracked and owned SDK spelling scans pass.
No renderer/provider/global-install/release operation was needed.
