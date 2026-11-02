#!/usr/bin/env bash
set -euo pipefail
MIN_VERSION="0.2.420"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/lmcode-compiler-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) WORK="$(cd "$WORK" && pwd -W | tr '\\' '/')" ;;
esac
candidate="${LMSCRIPT_BIN:-}"
if [[ -z "$candidate" ]]; then candidate="$(command -v lmscript || true)"; fi
if [[ -z "$candidate" ]]; then echo "error: no lmscript compiler found; install @lmctl-ai/lmctl >= $MIN_VERSION or set LMSCRIPT_BIN" >&2; exit 2; fi
# Qualify actual source compiler capabilities, not its package version.
SCHEMA="$WORK/implicit-std.schema.json"
PROBE="$WORK/probe.lms"
cat > "$SCHEMA" <<'JSON'
{"dsl.schema":2,"name":"implicit_std_probe","types":{},"functions":{},"context":{}}
JSON
python3 - "$SCHEMA" "$PROBE" "$WORK" <<'PY'
import json, pathlib, sys
schema, probe, work = sys.argv[1:]
def source(marker):
    return (f'p = {marker} schema={json.dumps(schema)};\n'
            'let value = <<~STD_PROBE;\n'
            '  raw ${not_an_outer_field} "quotes"\n'
            '  STD_PROBE\n'
            'let n = str_len(value);\n'
            f'end {marker}\n')
pathlib.Path(work, 'probe-lmcode.lms').write_text(source('_lmcode_'))
PY
"$candidate" --check "$WORK/probe-lmcode.lms" >"$WORK/compiler.out" 2>"$WORK/compiler.err" || { cat "$WORK/compiler.err" >&2; echo "error: compiler cannot check an _lmcode_ block against the host schema" >&2; exit 2; }
"$candidate" --lmcode-signatures "$SCHEMA" | grep -Fq 'fn str_len(string) -> int' || { echo "error: compiler inspection surface lacks implicit std fn str_len(string) -> int" >&2; exit 2; }
printf '%s\n' "$candidate"
