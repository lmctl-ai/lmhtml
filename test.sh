#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
BUILD_DIR="${BUILD_DIR:-build}"
HOST="$BUILD_DIR/lmhtml"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/lmhtml-test-XXXXXX")"
pass=0
ok(){ pass=$((pass+1)); echo "ok $pass - $1"; }
fail(){ echo "FAIL: $*" >&2; exit 1; }

LMSCRIPT="$(scripts/select-compiler.sh)"
COMPILER_SOURCE="$LMSCRIPT"
scripts/sync-sdk.sh --verify >/dev/null || fail "SDK header pin mismatch"
ok "SDK header matches sdk.lock"
scripts/sync-lexbor.sh --verify >/dev/null || fail "lexbor pin mismatch"
ok "third_party/lexbor matches lexbor.lock"

CMAKE_EXTRA=()
SANITIZING=0
if [[ "$BUILD_DIR" == *sanitize* ]]; then
  SANITIZING=1
  CMAKE_EXTRA=(-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
               -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
               -DLMHTML_FULL_STATIC=OFF)
fi
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release "${CMAKE_EXTRA[@]}" >"$WORK/cmake.log"
nice -n 19 cmake --build "$BUILD_DIR" -j2 >"$WORK/build.log"
ok "host build ($BUILD_DIR)"

"$LMSCRIPT" examples/check-links.lms build/check-links.dslb >"$WORK/compile.log" || fail "check-links compile with $COMPILER_SOURCE"
[ "$(od -An -t u1 -j 5 -N 1 build/check-links.dslb | tr -d ' ')" = 5 ] || fail "check-links was not compiled as bytecode v5"
ok "check-links.lms compiles with $COMPILER_SOURCE as bytecode v5"

"$HOST" --schema --json >"$WORK/embedded-schema.json" || fail "embedded schema print"
cmp schema/lmhtml.dsl.json "$WORK/embedded-schema.json" || fail "embedded schema differs from source bytes"
ok "--schema --json prints byte-identical embedded schema bytes"
"$HOST" --schema >"$WORK/schema-summary.txt" || fail "schema summary print"
python3 - "$WORK/schema-summary.txt" <<'PY'
import re,subprocess,sys
summary=open(sys.argv[1]).read()
sigs=subprocess.run(['build/lmhtml','--dsl-signatures'],capture_output=True,text=True).stdout.splitlines()
missing=[]
for line in sigs:
  if line.startswith('method '): needle=line[7:]
  elif line.startswith('fn '): needle=line[3:]
  elif line.startswith('context '): needle=line[8:]
  else: needle=line
  if needle not in summary: missing.append(needle)
assert not missing, missing
print('all', len(sigs), 'registry members listed in --schema summary')
PY
ok "--schema summary lists every registry member"
diff -u <("$LMSCRIPT" --dsl-signatures schema/lmhtml.dsl.json) <("$HOST" --dsl-signatures) || fail "host/compiler signatures differ"
ok "typed registry signatures match compiler schema signatures"

"$HOST" --dsl build/check-links.dslb >"$WORK/run.jsonl" || fail "check-links run"
[ "$(cat "$WORK/run.jsonl")" = '{"ok": true, "result": null}' ] || fail "unexpected result line"
ok "check-links bytecode runs"
cat > "$WORK/expected.txt" <<'EOF'
broken-link tests/fixtures/site/about.html:0: index.html#nope not found
broken-link tests/fixtures/site/index.html:0: missing.html not found
broken-link tests/fixtures/site/index.html:0: #gone not found
broken-link tests/fixtures/site/sub/page.html:0: ../nowhere.html not found
EOF
cmp "$WORK/expected.txt" build/links.txt || fail "text report differs from expected findings"
ok "broken links report matches expected findings exactly"
python3 - <<'PY'
import json
data=json.load(open('build/links.json'))
assert data['count']==4, data
files=[f['file'] for f in data['findings']]
assert files==['tests/fixtures/site/about.html','tests/fixtures/site/index.html','tests/fixtures/site/index.html','tests/fixtures/site/sub/page.html'], files
assert all(f['kind']=='broken-link' and f['line']==0 for f in data['findings'])
print('JSON report: 4 findings, ordered, well-formed')
PY
ok "JSON report matches expected findings"

cp build/links.txt "$WORK/links-first.txt"
"$HOST" --dsl build/check-links.dslb >/dev/null || fail "deterministic rerun"
cmp "$WORK/links-first.txt" build/links.txt || fail "report not deterministic"
ok "link check rerun is byte-identical (deterministic)"

"$HOST" --dsl build/check-links.dslb build/check-links.dslb >"$WORK/multi.jsonl" || fail "multi-bytecode invocation"
[ "$(wc -l < "$WORK/multi.jsonl" | tr -d ' ')" = 2 ] || fail "multiple bytecode files did not run in order"
ok "multiple bytecode files run sequentially against one embedded schema"

compile_chunk() { # name body
  cat > "$WORK/$1.lms" <<EOF2
p = _dsl_ schema="$HERE/schema/lmhtml.dsl.json";
$2
end _dsl_
savedsl(p, "$WORK/$1.dslb");
EOF2
  "$LMSCRIPT" "$WORK/$1.lms" "$WORK/$1.dslb" >/dev/null || fail "$1 compile"
}

set +e; out="$("$HOST" 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [[ "$out" == *'usage: lmhtml'* ]] || fail "bad args refusal (exit $code): $out"
ok "bad arguments refused (exit 2)"
set +e; out="$("$HOST" --schema schema/lmhtml.dsl.json 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [[ "$out" == *'usage: lmhtml'* ]] || fail "schema-file option accepted (exit $code): $out"
ok "--schema FILE form refused (exit 2)"
set +e; out="$("$HOST" --dsl build/check-links.dslb --bogus 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [ ! -s "$WORK/multi.jsonl.bak" ] || true
[ "$code" = 2 ] || fail "invalid --dsl argument accepted (exit $code): $out"
ok "invalid --dsl argument refused upfront (exit 2)"

compile_chunk bad-selector 'html.parse("<p>x</p>").select("[[[");'
set +e; out="$("$HOST" --dsl "$WORK/bad-selector.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'invalid selector'* ]] || fail "bad selector refusal (exit $code): $out"
ok "invalid CSS selector refused at run time (exit 3)"

compile_chunk out-of-range 'let d = html.parse("<p>x</p>"); d.select("p").at(5);'
set +e; out="$("$HOST" --dsl "$WORK/out-of-range.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'out of range'* ]] || fail "at() bounds refusal (exit $code): $out"
ok "NodeList.at out of range refused (exit 3)"

compile_chunk missing-file 'html.load("tests/fixtures/site/no-such.html");'
set +e; out="$("$HOST" --dsl "$WORK/missing-file.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'cannot read'* ]] || fail "missing file refusal (exit $code): $out"
ok "missing HTML file refused (exit 3)"

compile_chunk load-dir-as-file 'html.load("tests/fixtures/site");'
set +e; out="$("$HOST" --dsl "$WORK/load-dir-as-file.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'cannot read'* ]] || fail "directory loaded as a page (exit $code): $out"
compile_chunk load-device 'html.load("/dev/zero");'
set +e; out="$(timeout 10 "$HOST" --dsl "$WORK/load-device.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'cannot read'* ]] || fail "device file not refused (exit $code): $out"
ok "directories and device files refused by Html.load (exit 3)"

if [ "$(id -u)" != 0 ]; then
  mkdir -p "$WORK/locked/inner"; chmod 000 "$WORK/locked/inner"
  compile_chunk load-dir-locked 'html.load_dir("'"$WORK"'/locked");'
  set +e; out="$("$HOST" --dsl "$WORK/load-dir-locked.dslb" 2>&1)"; code=$?; set -e
  chmod 755 "$WORK/locked/inner"
  [ "$code" = 3 ] && [[ "$out" == *'cannot list'* ]] || fail "unreadable subdirectory not a clean error (exit $code): $out"
  ok "Html.load_dir on an unreadable subdirectory is a clean error (exit 3)"
fi

printf '<a href="a\xff\xfe\xc0\xafb\xed\xa0\x80\x01c\nd\x7f">x</a><a href="&quot;q&quot;\\ \xc3\xa9">y</a>' > "$WORK/bad-bytes.html"
compile_chunk report-escape 'let d = html.load("'"$WORK"'/bad-bytes.html"); let l = d.links();
for (i in range(0, l.len())) { report.add("k\"\n", l.at(i), 0, l.at(i)); }
report.write_json("'"$WORK"'/escape.json"); report.write_text("'"$WORK"'/escape.txt");'
"$HOST" --dsl "$WORK/report-escape.dslb" >/dev/null || fail "report escape run"
python3 - "$WORK" <<'PY'
import json,sys
raw=open(sys.argv[1]+'/escape.json','rb').read()
data=json.loads(raw.decode('utf-8'))  # strict: invalid UTF-8 raises
f=data['findings']
assert data['count']==2 and f[0]['kind']=='k"\n', f
assert f[0]['file']=='a����b���\x01c\nd\x7f', repr(f[0]['file'])
assert f[1]['file']=='"q"\\ é', repr(f[1]['file'])
lines=open(sys.argv[1]+'/escape.txt','rb').read().split(b'\n')
assert len(lines)==3 and lines[2]==b'', lines  # two findings, no forged lines
PY
ok "reports stay valid: invalid UTF-8 -> U+FFFD in JSON, control chars escaped in text"

compile_chunk link-policy 'let pages = html.load_dir("tests/fixtures/links");
for (i in range(0, pages.len())) {
  let page = pages.at(i); let links = page.links();
  for (j in range(0, links.len())) {
    let url = links.at(j);
    if (html.link_exists(page, url)) { report.add("ok", page.path(), 0, url + " -> " + html.resolve(page, url)); }
    else { report.add("broken", page.path(), 0, url + " -> " + html.resolve(page, url)); }
  }
}
report.write_text("'"$WORK"'/link-policy.txt");'
"$HOST" --dsl "$WORK/link-policy.dslb" >/dev/null || fail "link policy run"
L=tests/fixtures/links
cat > "$WORK/link-policy.expected" <<EOF
ok $L/based/base.html:0: index.html#s -> $L/sub/index.html#s
broken $L/based/base.html:0: page.html -> $L/sub/page.html
ok $L/based/external-base.html:0: anything.html -> anything.html
ok $L/index.html:0: sub/ -> $L/sub/index.html
ok $L/index.html:0: sub/#s -> $L/sub/index.html#s
ok $L/index.html:0: page.html?x=1#old -> $L/page.html#old
ok $L/index.html:0: my%20dir/p.html -> $L/my dir/p.html
ok $L/index.html:0: #caf%C3%A9 -> $L/index.html#caf%C3%A9
ok $L/index.html:0:   page.html   -> $L/page.html
ok $L/index.html:0: /page.html -> $L/page.html
ok $L/index.html:0: //cdn.example.com/x.js -> //cdn.example.com/x.js
ok $L/index.html:0: MAILTO:x@example.com -> MAILTO:x@example.com
ok $L/index.html:0: javascript:void(0) -> javascript:void(0)
ok $L/index.html:0: data:text/html,x -> data:text/html,x
ok $L/index.html:0: ?only=query -> $L/index.html
ok $L/index.html:0: #top -> $L/index.html#top
broken $L/index.html:0: PAGE.html -> $L/PAGE.html
broken $L/index.html:0: sub/../../index.html -> tests/fixtures/index.html
broken $L/index.html:0: /../index.html -> tests/fixtures/index.html
broken $L/index.html:0: nodir/ -> $L/nodir
broken $L/index.html:0: my%20dir/ -> $L/my dir/index.html
broken $L/index.html:0: page.html#nope -> $L/page.html#nope
broken $L/index.html:0: page%00.html -> $L/page\x00.html
EOF
diff -u "$WORK/link-policy.expected" "$WORK/link-policy.txt" || fail "link resolution policy"
ok "link policy: dirs, query, percent-encoding, base href, root, schemes, escapes"

compile_chunk not-implemented 'let d = html.parse("<p>x</p>"); html.diff(d, d);'
set +e; out="$("$HOST" --dsl "$WORK/not-implemented.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'not implemented yet'* ]] || fail "stub refusal (exit $code): $out"
ok "later-milestone members refuse cleanly (exit 3)"

set +e; out="$("$HOST" --dsl "$WORK/no-such.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'"phase": "load"'* ]] || fail "missing bytecode refusal (exit $code): $out"
ok "missing bytecode file refused at load (exit 3)"

compile_chunk stop-first 'report.add("x", "y", 0, "z"); report.write_text("'"$WORK"'/stop.txt");'
compile_chunk stop-fail 'html.load("nope.html");'
compile_chunk stop-later 'report.write_text("'"$WORK"'/later.txt");'
rm -f "$WORK/stop.txt" "$WORK/later.txt"
set +e; "$HOST" --dsl "$WORK/stop-first.dslb" "$WORK/stop-fail.dslb" "$WORK/stop-later.dslb" >/dev/null 2>&1; code=$?; set -e
[ "$code" = 3 ] && [ -f "$WORK/stop.txt" ] && [ ! -f "$WORK/later.txt" ] || fail "stop-on-failure semantics broken (exit $code)"
ok "shared state, ordered blocks, stop at first failure"

rm -f "$WORK/stop.txt"
set +e; out="$("$HOST" --dsl "$WORK/stop-first.dslb" "$WORK/no-such.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'"phase": "load"'* ]] && [ ! -f "$WORK/stop.txt" ] || fail "later missing bytecode did not stop the run before block 1 (exit $code): $out"
ok "all bytecode files load before the first block runs"

python3 - "$WORK" <<'PY'
import pathlib,sys
p=bytearray(pathlib.Path('build/check-links.dslb').read_bytes())
p[10] ^= 0xFF  # schema hash region
pathlib.Path(sys.argv[1],'hash-flip.dslb').write_bytes(bytes(p))
PY
set +e; out="$("$HOST" --dsl "$WORK/hash-flip.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'schema'* ]] || fail "schema-hash mismatch refusal (exit $code): $out"
ok "schema-hash mismatch refused (exit 3)"

if [ "$SANITIZING" = 0 ]; then
  LDD_OUT="$(ldd "$HOST" 2>&1 || true)"
  [[ "$LDD_OUT" == *'not a dynamic executable'* ]] || fail "host is not fully static: $LDD_OUT"
  ok "host is fully static ($(stat -c %s "$HOST") bytes, ldd clean)"
fi

echo "PASS: $pass checks"
