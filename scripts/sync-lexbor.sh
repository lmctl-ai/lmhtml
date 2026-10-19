#!/usr/bin/env bash
# Fetch lexbor into third_party/lexbor (git-ignored) at the commit pinned in lexbor.lock.
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK="$HERE/lexbor.lock"
DIR="$HERE/third_party/lexbor"
[ -f "$LOCK" ] || { echo "lexbor.lock missing" >&2; exit 1; }
repo="$(sed -n 's/^repo=//p' "$LOCK")"
commit="$(sed -n 's/^commit=//p' "$LOCK")"
if [ "${1:-}" = "--verify" ]; then
  [ -d "$DIR/.git" ] || { echo "third_party/lexbor missing: run scripts/sync-lexbor.sh" >&2; exit 1; }
  have="$(git -C "$DIR" rev-parse HEAD)"
  [ "$have" = "$commit" ] || { echo "third_party/lexbor at $have, lexbor.lock pins $commit: run scripts/sync-lexbor.sh" >&2; exit 1; }
  echo "third_party/lexbor matches lexbor.lock ($commit)"
  exit 0
fi
if [ ! -d "$DIR/.git" ]; then
  mkdir -p "$DIR"
  git -C "$DIR" init -q
  git -C "$DIR" remote add origin "$repo"
fi
git -C "$DIR" fetch -q --depth 1 origin "$commit"
git -C "$DIR" checkout -q FETCH_HEAD
echo "synced third_party/lexbor at $commit"
