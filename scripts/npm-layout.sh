#!/usr/bin/env bash
# Lay out an esbuild-style npm package set from built binaries. Nothing is
# published: this only writes package directories.
# Usage: scripts/npm-layout.sh <artifacts-dir> <out-dir> <version>
# <artifacts-dir> contains lmhtml-<platform>[.exe] files, e.g. lmhtml-linux-x64.
set -euo pipefail
ART="${1:?artifacts dir}"
OUT="${2:?output dir}"
VER="${3:-0.0.0}"

PLATFORMS="linux-x64 linux-arm64 macos-x64 macos-arm64 windows-x64"
npm_os() { case "$1" in macos*) echo darwin;; windows*) echo win32;; *) echo "${1%%-*}";; esac; }
npm_cpu() { echo "${1##*-}"; }

rm -rf "$OUT"
mkdir -p "$OUT/lmhtml/bin" "$OUT/@lmhtml"

# Main package: bin shim resolves the per-platform optional dependency.
cat > "$OUT/lmhtml/package.json" <<EOF
{
  "name": "lmhtml",
  "version": "$VER",
  "description": "Static native CLI for bulk HTML work (lexbor + lmscript _dsl_ bytecode)",
  "license": "Apache-2.0",
  "bin": { "lmhtml": "bin/lmhtml.js" },
  "optionalDependencies": {
$(i=0; for p in $PLATFORMS; do
  [ $i -gt 0 ] && printf ',\n'
  printf '    "@lmhtml/%s": "%s"' "$p" "$VER"
  i=$((i+1))
done)
  }
}
EOF

cat > "$OUT/lmhtml/bin/lmhtml.js" <<'EOF'
#!/usr/bin/env node
const { execFileSync } = require('node:child_process');
const path = require('node:path');
const map = {
  'linux-x64': '@lmhtml/linux-x64',
  'linux-arm64': '@lmhtml/linux-arm64',
  'darwin-x64': '@lmhtml/macos-x64',
  'darwin-arm64': '@lmhtml/macos-arm64',
  'win32-x64': '@lmhtml/windows-x64',
};
const key = `${process.platform}-${process.arch}`;
const pkg = map[key];
if (!pkg) {
  console.error(`lmhtml: unsupported platform ${key}`);
  process.exit(1);
}
const binName = process.platform === 'win32' ? 'lmhtml.exe' : 'lmhtml';
const bin = path.join(path.dirname(require.resolve(`${pkg}/package.json`)), binName);
try {
  execFileSync(bin, process.argv.slice(2), { stdio: 'inherit' });
} catch (error) {
  process.exit(typeof error.status === 'number' ? error.status : 1);
}
EOF
chmod +x "$OUT/lmhtml/bin/lmhtml.js"

# Per-platform packages carrying the raw binary.
for plat in $PLATFORMS; do
  src="$ART/lmhtml-$plat"
  bin="lmhtml"
  [ -f "$src.exe" ] && { src="$src.exe"; bin="lmhtml.exe"; }
  [ -f "$src" ] || { echo "missing artifact for $plat" >&2; exit 1; }
  dir="$OUT/@lmhtml/$plat"
  mkdir -p "$dir"
  cp "$src" "$dir/$bin"
  chmod +x "$dir/$bin"
  cat > "$dir/package.json" <<EOF
{
  "name": "@lmhtml/$plat",
  "version": "$VER",
  "description": "lmhtml binary for $plat",
  "license": "Apache-2.0",
  "os": [ "$(npm_os "$plat")" ],
  "cpu": [ "$(npm_cpu "$plat")" ],
  "files": [ "$bin" ]
}
EOF
done
echo "npm package set laid out in $OUT (main + $(echo $PLATFORMS | wc -w) platform packages)"
