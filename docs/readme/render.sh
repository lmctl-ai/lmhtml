#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

compiler="${LMSCRIPT_BIN:-$(scripts/select-compiler.sh)}"
cache="${LMSCRIPT_TOOL_CACHE:-${HOME}/.cache/lmscript/tools}"
host_for() {
  local tool="$1" fallback="$2"
  local cached="$cache/github/lmctl-ai/$tool/latest/$tool"
  if [[ -x "$cached" ]]; then printf '%s\n' "$cached"
  elif [[ -x "${fallback}" ]]; then printf '%s\n' "$fallback"
  elif command -v "$tool" >/dev/null 2>&1; then command -v "$tool"
  else echo "error: $tool release binary not found; compile an install-enabled README script first" >&2; return 2
  fi
}
lmhtml_bin="${LMHTML_BIN:-$(host_for lmhtml build/lmhtml)}"
lmdraw_bin="${LMDRAW_BIN:-$(host_for lmdraw build/lmdraw)}"
want_lmhtml="lmhtml $(sed -n 's/^project(lmhtml VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
[[ "$($lmhtml_bin --version)" == "$want_lmhtml" ]] || { echo "error: expected $want_lmhtml (the version in CMakeLists.txt)" >&2; exit 2; }
[[ "$($lmdraw_bin --version)" == "lmdraw 0.2.0" ]] || { echo "error: expected lmdraw 0.2.0" >&2; exit 2; }

# Each script runs its program with run_lmcode; put the chosen hosts first on PATH.
PATH="$(dirname "$lmhtml_bin"):$(dirname "$lmdraw_bin"):$PATH"
export PATH
mkdir -p build/rendered examples/renders
examples=(check-links class-typos diff-pages make-template rewrite-nav)
for name in "${examples[@]}"; do
  "$compiler" "examples/$name.lms" >/dev/null
done

for name in hero-light hero-dark architecture-light architecture-dark; do
  "$compiler" "docs/readme/$name.lms" >/dev/null
  cp "build/rendered/readme-$name.png" "docs/readme/$name.png"
done

render_card() {
  local title="$1" report="$2" slug="$3"
  "$compiler" docs/readme/gallery-card.lms "$title" "$report" "build/rendered/readme-$slug.png" >/dev/null
  cp "build/rendered/readme-$slug.png" "examples/renders/$slug.png"
}
render_card "LINK AUDIT" build/links.txt check-links
render_card "CLASS + ID AUDIT" build/classes.txt class-typos
render_card "STRUCTURAL PAGE DIFF" build/diff.txt diff-pages
render_card "SHARED TEMPLATE" build/template-report.txt make-template
render_card "SHARED NAVIGATION REWRITE" build/rewrite-report.txt rewrite-nav

python3 - <<'PY'
from pathlib import Path
images = [Path("docs/readme") / f"{name}.png" for name in (
    "hero-light", "hero-dark", "architecture-light", "architecture-dark"
)]
images += [Path("examples/renders") / f"{name}.png" for name in (
    "check-links", "class-typos", "diff-pages", "make-template", "rewrite-nav"
)]
for image in images:
    if not image.is_file() or not image.stat().st_size:
        raise SystemExit(f"missing README artwork: {image}")
    if image.stat().st_size > 400_000:
        raise SystemExit(f"README image exceeds 400 KB: {image} ({image.stat().st_size} bytes)")
print(f"README artwork ready: {len(images)} images, each under 400 KB")
PY
