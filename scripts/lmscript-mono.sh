#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec node --experimental-strip-types "$HERE/scripts/lmscript-mono.mjs" "$@"
