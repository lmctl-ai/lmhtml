#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
BUILD_DIR="${BUILD_DIR:-build}"
HOST="$BUILD_DIR/lmhtml"
UNAME_S="$(uname -s)"
case "$UNAME_S" in MINGW*|MSYS*|CYGWIN*) HOST="$HOST.exe";; esac
# GNU timeout is `gtimeout` from coreutils on macOS.
TIMEOUT_CMD=timeout
command -v timeout >/dev/null 2>&1 || TIMEOUT_CMD=gtimeout
stat_size(){ stat -c %s "$1" 2>/dev/null || stat -f%z "$1"; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/lmhtml-test-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
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
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release ${CMAKE_EXTRA:+"${CMAKE_EXTRA[@]}"} >"$WORK/cmake.log" 2>&1 ||
  { tail -30 "$WORK/cmake.log" >&2; fail "cmake configure ($BUILD_DIR)"; }
nice -n 19 cmake --build "$BUILD_DIR" -j2 >"$WORK/build.log" 2>&1 ||
  { tail -30 "$WORK/build.log" >&2; fail "cmake build ($BUILD_DIR)"; }
ok "host build ($BUILD_DIR)"

"$LMSCRIPT" examples/check-links.lms build/check-links.dslb >"$WORK/compile.log" || fail "check-links compile with $COMPILER_SOURCE"
"$LMSCRIPT" examples/class-typos.lms build/class-typos.dslb >>"$WORK/compile.log" || fail "class-typos compile with $COMPILER_SOURCE"
"$LMSCRIPT" examples/diff-pages.lms build/diff-pages.dslb >>"$WORK/compile.log" || fail "diff-pages compile with $COMPILER_SOURCE"
"$LMSCRIPT" examples/make-template.lms build/make-template.dslb >>"$WORK/compile.log" || fail "make-template compile with $COMPILER_SOURCE"
"$LMSCRIPT" examples/rewrite-nav.lms build/rewrite-nav.dslb >>"$WORK/compile.log" || fail "rewrite-nav compile with $COMPILER_SOURCE"
for f in check-links class-typos diff-pages make-template rewrite-nav; do
  [ "$(od -An -t u1 -j 5 -N 1 "build/$f.dslb" | tr -d ' ')" = 5 ] || fail "$f was not compiled as bytecode v5"
done
ok "examples compile with $COMPILER_SOURCE as bytecode v5"

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

"$HOST" --dsl build/class-typos.dslb >"$WORK/typos.jsonl" || fail "class-typos run"
cat > "$WORK/expected-typos.txt" <<'EOF'
class-undefined site:0: usde (did you mean used?)
class-unused site:0: footr
id-unused site:0: teem (did you mean team?)
EOF
cmp "$WORK/expected-typos.txt" build/classes.txt || fail "class/id typo report differs from expected findings"
python3 - <<'PY'
import json
data=json.load(open('build/classes.json'))
kinds=[f['kind'] for f in data['findings']]
assert kinds==['class-undefined','class-unused','id-unused'], kinds
print('typo JSON report: 3 findings, well-formed')
PY
ok "class/id typo findings match expected exactly (text and JSON)"

"$HOST" --dsl build/diff-pages.dslb >"$WORK/diff.jsonl" || fail "diff-pages run"
cat > "$WORK/expected-diff.txt" <<'EOF'
diff-added tests/fixtures/diff/b.html:0: html > body:nth-child(2) > ul:nth-child(2) > li:nth-child(3)
diff-changed tests/fixtures/diff/a.html:0: html > body:nth-child(2) > p.intro:nth-child(3): attributes differ: class
diff-changed tests/fixtures/diff/a.html:0: html > body:nth-child(2) > p.intro:nth-child(3): text differs
diff-removed tests/fixtures/diff/a.html:0: html > body:nth-child(2) > div.box:nth-child(4)
summary a-vs-b:0: 4 differences
summary a-vs-noise:0: 0 differences
EOF
cmp "$WORK/expected-diff.txt" build/diff.txt || fail "structural diff report differs from expected findings"
python3 - <<'PY'
import json
data=json.load(open('build/diff.json'))
kinds=[f['kind'] for f in data['findings']]
assert kinds==['diff-added','diff-changed','diff-changed','diff-removed','summary','summary'], kinds
print('diff JSON report: 4 structural findings + 2 summaries')
PY
ok "structural diff: known edits found exactly, whitespace noise ignored, hash stable"

"$HOST" --dsl build/make-template.dslb >"$WORK/tpl.jsonl" || fail "make-template run"
cat > "$WORK/expected-template.html" <<'EOF'
<!DOCTYPE html>
<html><head><title>Site</title><style>.x { color: red; }</style></head><body><header class="site-header"><h1>Site</h1><p class="tag">Tagline</p></header><nav class="site-nav"><a href="p1.html">P1</a><a href="p2.html">P2</a><a href="p3.html">P3</a></nav><!-- slot:main-1 --><footer class="site-footer"><span>Copyright</span><span>Legal</span></footer></body></html>
EOF
cmp "$WORK/expected-template.html" build/template.html || fail "emitted template differs from expected"
cat > "$WORK/expected-tpl-report.txt" <<'EOF'
common-part build/template.html:0: html > head:nth-child(1) shared by 5 of 5 pages
common-part build/template.html:0: html > body:nth-child(2) > header.site-header:nth-child(1) shared by 5 of 5 pages
common-part build/template.html:0: html > body:nth-child(2) > nav.site-nav:nth-child(2) shared by 4 of 5 pages
common-part build/template.html:0: html > body:nth-child(2) > footer.site-footer:nth-child(4) shared by 4 of 5 pages
summary template:0: 1 slots for 5 pages
EOF
cmp "$WORK/expected-tpl-report.txt" build/template-report.txt || fail "template findings differ from expected"
ok "common parts across 5 pages (2 drifted) and template with 1 slot, byte-exact"

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
set +e; out="$("$TIMEOUT_CMD" 10 "$HOST" --dsl "$WORK/load-device.dslb" 2>&1)"; code=$?; set -e
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
# Case-insensitive filesystems (APFS, NTFS) resolve PAGE.html to page.html;
# the case-sensitive-server expectation only holds where the FS distinguishes.
if [ -f tests/fixtures/links/PAGE.html ]; then
  sed 's|^broken \(.*PAGE.html.*\)$|ok \1|' "$WORK/link-policy.expected" > "$WORK/link-policy.expected.adapted"
  mv "$WORK/link-policy.expected.adapted" "$WORK/link-policy.expected"
fi
diff -u "$WORK/link-policy.expected" "$WORK/link-policy.txt" || fail "link resolution policy"
ok "link policy: dirs, query, percent-encoding, base href, root, schemes, escapes"

E=tests/fixtures/diff-edge
body=''
for t in text pre bool move path; do body="$body
report.add(\"summary\", \"$t\", html.diff(html.load(\"$E/$t-a.html\"), html.load(\"$E/$t-b.html\")), \"\");"; done
compile_chunk diff-edge "$body
report.write_text(\"$WORK/diff-edge.txt\");"
"$HOST" --dsl "$WORK/diff-edge.dslb" >/dev/null || fail "diff edge run"
cat > "$WORK/diff-edge.expected" <<EOF
diff-changed $E/text-a.html:0: html > body:nth-child(2) > p:nth-child(1): text differs
summary text:1: 
diff-changed $E/pre-a.html:0: html > body:nth-child(2) > pre:nth-child(1): text differs
summary pre:1: 
summary bool:0: 
diff-removed $E/move-a.html:0: html > body:nth-child(2) > section:nth-child(3)
diff-added $E/move-b.html:0: html > body:nth-child(2) > section:nth-child(1)
summary move:2: 
diff-added $E/path-b.html:0: html > body:nth-child(2) > section:nth-child(3) > p:nth-child(2)
diff-added $E/path-b.html:0: html > body:nth-child(2) > nav:nth-child(1)
summary path:2: 
EOF
diff -u "$WORK/diff-edge.expected" "$WORK/diff-edge.txt" || fail "diff edge cases"
ok "diff: text position, <pre> whitespace, boolean attrs, moves, right-side paths"
python3 - "$WORK" <<'PY'
import sys
w=sys.argv[1]
open(w+'/wide-a.html','w').write('<ul>'+''.join('<li>a%d</li>'%i for i in range(20000))+'</ul>')
open(w+'/wide-b.html','w').write('<ul>'+''.join('<li>b%d</li>'%i for i in range(20000))+'</ul>')
PY
compile_chunk diff-wide 'report.add("n", "", html.diff(html.load("'"$WORK"'/wide-a.html"), html.load("'"$WORK"'/wide-b.html")), ""); report.write_text("'"$WORK"'/wide.txt");'
# ASan reserves terabytes of address space: the memory bound is release-only.
VMEM_LIMIT=1000000; [ "$SANITIZING" = 1 ] && VMEM_LIMIT=unlimited
( ulimit -v "$VMEM_LIMIT"; "$TIMEOUT_CMD" 60 "$HOST" --dsl "$WORK/diff-wide.dslb" >/dev/null ) || fail "20k x 20k sibling diff exceeded 1 GB / 60 s"
[ "$(tail -1 "$WORK/wide.txt")" = "n :20000: " ] || fail "wide diff count: $(tail -1 "$WORK/wide.txt")"
ok "diff of 20k x 20k distinct siblings stays bounded (LCS cell cap)"

T=tests/fixtures/tpl-edge
compile_chunk tpl-edge 'report.add("slots", "", html.write_template(html.load_dir("'"$T"'"), 2, "'"$WORK"'/tpl-edge.html"), ""); report.write_text("'"$WORK"'/tpl-edge.txt");'
"$HOST" --dsl "$WORK/tpl-edge.dslb" >/dev/null || fail "tpl-edge run"
cat > "$WORK/tpl-edge.expected" <<'EOF'
<!DOCTYPE html>
<html lang="en"><!-- slot:head-1 --><body class="p1"><!-- slot:text-2 --><header><nav><a href="/">Home</a><a href="/a">A</a></nav></header><!-- slot:main-3 --><footer><p>Copyright 2026 Example Corp, all rights reserved.</p></footer></body></html>
EOF
cmp "$WORK/tpl-edge.expected" "$WORK/tpl-edge.html" || fail "template edge: doctype kept, varying body text slotted"
[ "$(tail -1 "$WORK/tpl-edge.txt")" = "slots :3: " ] || fail "template edge slot count: $(tail -1 "$WORK/tpl-edge.txt")"
compile_chunk tpl-k-too-big 'html.write_template(html.load_dir("'"$T"'"), 4, "'"$WORK"'/never.html");'
set +e; out="$("$HOST" --dsl "$WORK/tpl-k-too-big.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'exceeds the 3 pages'* ]] && [ ! -e "$WORK/never.html" ] || fail "k > pages not refused (exit $code): $out"
ok "template: doctype kept, varying text slotted (no page words leak), k > pages refused"

compile_chunk strlib 'let s = "Hello World";
if (!str_starts_with(s, "Hello")) { report.add("bug", "f", 0, "starts_with"); }
if (!str_ends_with(s, "World")) { report.add("bug", "f", 0, "ends_with"); }
if (!str_contains(s, "lo Wo")) { report.add("bug", "f", 0, "contains"); }
if (str_lower(s) != "hello world") { report.add("bug", "f", 0, "lower"); }
if (str_substr(s, 6, 3) != "Wor") { report.add("bug", "f", 0, "substr"); }
if (str_substr(s, -5, 100) != s) { report.add("bug", "f", 0, "substr clamp"); }
if (str_len(s) != 11) { report.add("bug", "f", 0, "len"); }
if (str_union(str_list(), str_list()).len() != 0) { report.add("bug", "f", 0, "union empty"); }
report.add("strlib", "ok", 0, "checked"); report.write_text("'"$WORK"'/strlib.txt");'
"$HOST" --dsl "$WORK/strlib.dslb" >/dev/null || fail "strlib chunk run"
[ "$(cat "$WORK/strlib.txt")" = "strlib ok:0: checked" ] || fail "str_* helpers misbehave: $(cat "$WORK/strlib.txt")"
ok "str_* helpers (starts/ends/contains/lower/substr/len) behave"

compile_chunk glob 'let ok = str_match("language-rust", "language-*");
if (!ok) { report.add("bug", "f", 0, "star"); }
if (str_match("language-rust", "language-?")) { report.add("bug", "f", 0, "qmark"); }
if (!str_match("fa-pencil", "fa-p*[il]")) { report.add("bug", "f", 0, "class"); }
if (str_match("fa-pencil", "fa-p[!e]*l")) { report.add("bug", "f", 0, "negclass"); }
if (!str_match("a/b/c.html", "*html")) { report.add("bug", "f", 0, "star slash"); }
if (str_match("abc", "a*d")) { report.add("bug", "f", 0, "backtrack"); }
report.add("glob", "ok", 0, "checked"); report.write_text("'"$WORK"'/glob.txt");'
"$HOST" --dsl "$WORK/glob.dslb" >/dev/null || fail "glob chunk run"
[ "$(cat "$WORK/glob.txt")" = "glob ok:0: checked" ] || fail "str_match glob misbehaves: $(cat "$WORK/glob.txt")"
ok "str_match glob: * ? [...] [!...] and backtracking behave"

compile_chunk link-kind 'let pages = html.load_dir("tests/fixtures/site");
let d = pages.at(0);
report.add("kind", "page", 0, html.link_kind(d, "index.html"));
report.add("kind", "asset", 0, html.link_kind(d, "logo.png"));
report.add("kind", "anchor", 0, html.link_kind(d, "#team"));
report.add("kind", "external", 0, html.link_kind(d, "https://example.com/"));
report.add("kind", "special", 0, html.link_kind(d, "mailto:a@b.c"));
report.add("kind", "outside", 0, html.link_kind(d, "../escaped.html"));
report.write_text("'"$WORK"'/link-kind.txt");'
"$HOST" --dsl "$WORK/link-kind.dslb" >/dev/null || fail "link-kind run"
cat > "$WORK/link-kind.expected" <<'EOF'
kind page:0: internal-page
kind asset:0: internal-asset
kind anchor:0: anchor
kind external:0: external
kind special:0: special
kind outside:0: outside-root
EOF
cmp "$WORK/link-kind.expected" "$WORK/link-kind.txt" || fail "link_kind classification wrong: $(cat "$WORK/link-kind.txt")"
ok "link_kind classifies page/asset/anchor/external/special/outside-root"

compile_chunk load-glob 'let all = html.load_dir("tests/fixtures/site");
let no_sub = html.load_dir("tests/fixtures/site", "", "sub/*");
let only_index = html.load_dir("tests/fixtures/site", "index.html", "");
report.add("g", "all", 0, str(all.len()));
report.add("g", "nosub", 0, str(no_sub.len()));
report.add("g", "only", 0, str(only_index.len()));
report.add("g", "classes", 0, str(all.classes_used().len()));
report.add("g", "css", 0, str(all.css_classes().len()));
report.add("g", "ids", 0, str(all.ids().len()));
report.add("g", "cssids", 0, str(all.css_ids().len()));
report.write_text("'"$WORK"'/load-glob.txt");'
"$HOST" --dsl "$WORK/load-glob.dslb" >"$WORK/load-glob.out" || { cat "$WORK/load-glob.out" >&2; fail "load-glob run"; }
cat > "$WORK/load-glob.expected" <<'EOF'
g all:0: 3
g nosub:0: 2
g only:0: 1
g classes:0: 3
g css:0: 3
g ids:0: 2
g cssids:0: 1
EOF
cmp "$WORK/load-glob.expected" "$WORK/load-glob.txt" || fail "load_dir globs / DocList aggregation wrong: $(cat "$WORK/load-glob.txt")"
ok "load_dir include/exclude globs and DocList site-wide classes_used/css_classes/ids/css_ids"

python3 - "$WORK" <<'PY'
import pathlib,sys
w=pathlib.Path(sys.argv[1])
(w/'deep.html').write_text('<html><body>' + '<div>'*600 + 'x' + '</div>'*600 + '</body></html>')
(w/'deep-ok.html').write_text('<html><body>' + '<div>'*500 + 'x' + '</div>'*500 + '</body></html>')
PY
compile_chunk deep-load 'html.load("'"$WORK"'/deep.html");'
set +e; out="$("$HOST" --dsl "$WORK/deep-load.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *"nesting depth exceeds 512 in $WORK/deep.html"* ]] || fail "depth refusal (exit $code): $out"
ok "document nested deeper than 512 refused, naming the file (exit 3)"
compile_chunk deep-ok 'let d = html.load("'"$WORK"'/deep-ok.html"); report.add("ok", d.path(), 0, str(d.select("div").len())); report.write_text("'"$WORK"'/deep-ok.txt");'
"$HOST" --dsl "$WORK/deep-ok.dslb" >/dev/null || fail "500-deep document must parse"
[ "$(cat "$WORK/deep-ok.txt")" = "ok $WORK/deep-ok.html:0: 500" ] || fail "500-deep document content wrong: $(cat "$WORK/deep-ok.txt")"
ok "document at 500 nesting levels parses fine (limit is 512)"
# Prescan: unclosed siblings are not nesting; stray quotes, odd comments and
# nested lists must not hide real depth from it (a stall, not a refusal).
python3 - "$WORK" <<'PY'
import sys
w=sys.argv[1]
open(w+'/flat-li.html','w').write('<ul>'+'<li>x'*600+'</ul><table>'+'<tr><td>x'*600+'</table>'+'<p>a'*600+'<textarea>'+'<div>'*600+'</textarea>')
for name,body in {'quote':'<div x">'+'<div>'*300000,'comment':'<!-->'+'<div>'*300000+'-->','lists':'<ul><li>'*100000,'tables':'<table><tr><td>'*100000}.items():
  open(w+'/deep-'+name+'.html','w').write(body)
PY
compile_chunk flat-li 'html.load("'"$WORK"'/flat-li.html");'
"$HOST" --dsl "$WORK/flat-li.dslb" >/dev/null || fail "600 unclosed <li>/<td>/<p> falsely refused as deep nesting"
for name in quote comment lists tables; do
  compile_chunk "deep-$name" 'html.load("'"$WORK/deep-$name.html"'");'
  set +e; out="$("$TIMEOUT_CMD" 10 "$HOST" --dsl "$WORK/deep-$name.dslb" 2>&1)"; code=$?; set -e
  [ "$code" = 3 ] && [[ "$out" == *'nesting depth exceeds 512'* ]] || fail "depth prescan bypass via $name (exit $code): $out"
done
ok "depth prescan: unclosed siblings allowed; quote/comment/list/table bypasses refused fast"

compile_chunk tpl-bad-k 'let d = html.load_dir("tests/fixtures/tpl"); html.write_template(d, 0, "'"$WORK"'/t.html");'
set +e; out="$("$HOST" --dsl "$WORK/tpl-bad-k.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'k must be at least 1'* ]] || fail "write_template k=0 refusal (exit $code): $out"
ok "write_template refuses k < 1 (exit 3)"

compile_chunk ext-css 'let d = html.load("tests/fixtures/realworld/ext-css/page.html");
let defined = d.css_classes();
let used = d.classes_used();
let missing = str_difference(used, defined);
for (j in range(0, missing.len())) { report.add("class-undefined", d.path(), 0, missing.at(j)); }
let ids_defined = d.css_ids();
let ids_used = d.ids();
let ids_missing = str_difference(ids_defined, ids_used);
for (m in range(0, ids_missing.len())) { report.add("id-unused", d.path(), 0, ids_missing.at(m)); }
report.write_text("'"$WORK"'/ext-css.txt");'
"$HOST" --dsl "$WORK/ext-css.dslb" >/dev/null || fail "ext-css run"
cat > "$WORK/ext-css.expected" <<'EOF'
class-undefined tests/fixtures/realworld/ext-css/page.html:0: frm-ext
id-unused tests/fixtures/realworld/ext-css/page.html:0: ext-id
id-unused tests/fixtures/realworld/ext-css/page.html:0: media-id
EOF
cmp "$WORK/ext-css.expected" "$WORK/ext-css.txt" || fail "external stylesheet + @media harvesting wrong: $(cat "$WORK/ext-css.txt")"
ok "css_classes/css_ids read linked stylesheets and @media blocks"

compile_chunk srcset 'let d = html.load("tests/fixtures/realworld/srcset/page.html");
let links = d.links();
for (j in range(0, links.len())) {
  let url = links.at(j);
  if (!html.link_exists(d, url)) { report.add("broken-link", d.path(), 0, url); }
}
report.write_text("'"$WORK"'/srcset.txt");'
"$HOST" --dsl "$WORK/srcset.dslb" >/dev/null || fail "srcset run"
echo 'broken-link tests/fixtures/realworld/srcset/page.html:0: missing.png' > "$WORK/srcset.expected"
cmp "$WORK/srcset.expected" "$WORK/srcset.txt" || fail "srcset link collection wrong: $(cat "$WORK/srcset.txt")"
ok "img srcset URLs collected for link checking (data: srcset ignored)"

"$HOST" --dsl build/rewrite-nav.dslb >"$WORK/rewrite.jsonl" || fail "rewrite-nav run"
cat > "$WORK/expected-rewrite.txt" <<'EOF'
diff-changed build/rewritten/p0.html:0: html > body:nth-child(2) > main:nth-child(3) > h2:nth-child(1): text differs
diff-changed build/rewritten/p0.html:0: html > body:nth-child(2) > main:nth-child(3) > p:nth-child(2): text differs
diff-changed build/rewritten/p0.html:0: html > body:nth-child(2) > main:nth-child(3) > h2:nth-child(1): text differs
diff-changed build/rewritten/p0.html:0: html > body:nth-child(2) > main:nth-child(3) > p:nth-child(2): text differs
converged p0-vs-p3:0: 2 differences after rewrite
converged p0-vs-p4:0: 2 differences after rewrite
EOF
cmp "$WORK/expected-rewrite.txt" build/rewrite-report.txt || fail "rewrite report differs from expected"
grep -q '^<!DOCTYPE html>' build/rewritten/p0.html || fail "saved page lost the doctype"
ok "nav/footer rewritten across 5 pages, pages converge (M3 re-diff), doctype kept"

cp -r build/rewritten "$WORK/rewritten-first"
"$HOST" --dsl build/rewrite-nav.dslb >/dev/null || fail "rewrite-nav rerun"
diff -r "$WORK/rewritten-first" build/rewritten || fail "rewrite rerun not idempotent"
ok "rewrite re-run is byte-identical (idempotent)"

compile_chunk save-escape 'let d = html.parse("<p>x</p>"); html.save(d, "'"$WORK"'/escape.html");'
set +e; out="$("$HOST" --dsl "$WORK/save-escape.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'outside the output root'* ]] && [ ! -f "$WORK/escape.html" ] || fail "save escape refusal (exit $code): $out"
ok "Html.save refuses paths outside the output root (exit 3, nothing written)"

# Save safety: tests run with cwd = $WORK/outroot (the output root).
OUTROOT="$WORK/outroot"; mkdir -p "$OUTROOT/sub" "$WORK/outside"
echo victim > "$WORK/outside/victim.txt"
ln -s "$WORK/outside/victim.txt" "$OUTROOT/sub/page.html.tmp-lmhtml"   # old fixed temp name
printf '<p>old</p>' > "$OUTROOT/mode.html"; chmod 640 "$OUTROOT/mode.html"
compile_chunk save-safety 'let d = html.parse("<p>new</p>"); html.save(d, "sub/page.html"); html.save(d, "mode.html");'
( cd "$OUTROOT" && "$HERE/$HOST" --dsl "$WORK/save-safety.dslb" >/dev/null ) || fail "save-safety run"
[ "$(cat "$WORK/outside/victim.txt")" = victim ] || fail "save wrote through a planted temp-name symlink"
[ -f "$OUTROOT/sub/page.html" ] && [ ! -L "$OUTROOT/sub/page.html" ] || fail "saved page is not a regular file"
case "$UNAME_S" in
  MINGW*|MSYS*|CYGWIN*) : ;; # NTFS has no POSIX modes; the Windows save path documents the reduced guarantee
  *)
    [ "$(stat -c %a "$OUTROOT/mode.html" 2>/dev/null || stat -f%Lp "$OUTROOT/mode.html")" = 640 ] || fail "save did not keep the replaced file's mode"
    ;;
esac
compile_chunk save-dir 'html.save(html.parse("<p>x</p>"), "sub");'
compile_chunk save-slash 'html.save(html.parse("<p>x</p>"), "newdir/");'
for t in save-dir save-slash; do
  set +e; out="$(cd "$OUTROOT" && "$HERE/$HOST" --dsl "$WORK/$t.dslb" 2>&1)"; code=$?; set -e
  [ "$code" = 3 ] || fail "$t not refused (exit $code): $out"
done
[ ! -e "$OUTROOT/newdir" ] || fail "refused save left a directory behind"
python3 -c "print('<p>' + 'x' * 300000 + '</p>')" > "$WORK/big-page.html"
compile_chunk save-efbig 'html.save(html.load("'"$WORK"'/big-page.html"), "big.html");'
set +e; out="$(cd "$OUTROOT" && trap '' XFSZ && ulimit -f 100 && "$HERE/$HOST" --dsl "$WORK/save-efbig.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'failed writing'* ]] || fail "short write not reported (exit $code): $out"
[ -z "$(ls -A "$OUTROOT" | grep -v -e '^sub$' -e '^mode.html$')" ] || fail "failed save left files: $(ls -A "$OUTROOT")"
ok "save: planted temp symlink harmless, mode kept, dir/slash refused, failed write leaves no temp"

# Mutation guards: deep fragments (parser stall / recursion crash), attribute
# names that serialize as markup, splices that would replace <head>/<body>.
expect_refusal() { # name body message
  compile_chunk "$1" "$2"
  set +e; out="$("$TIMEOUT_CMD" 20 "$HOST" --dsl "$WORK/$1.dslb" 2>&1)"; code=$?; set -e
  [ "$code" = 3 ] && [[ "$out" == *"$3"* ]] || fail "$1 not refused cleanly (exit $code): $out"
}
expect_refusal frag-deep 'let deep = "<div>"; for (i in range(0, 17)) { deep = deep + deep; } html.parse("<p>x</p>").select("p").at(0).replace_with(deep);' 'nesting depth exceeds 512'
expect_refusal frag-grow 'let d = html.parse("<p>x</p>"); let deep = "<div>"; for (i in range(0, 8)) { deep = deep + deep; } for (k in range(0, 4)) { let all = d.select("div"); let t = d.select("p"); if (all.len() > 0) { t = all; } t.at(t.len() - 1).replace_with(deep); }' 'nest deeper than 512'
expect_refusal attr-inject 'html.parse("<p>x</p>").select("p").at(0).set_attr("x><script>alert(1)</script", "");' 'invalid attribute name'
expect_refusal attr-space 'html.parse("<p>x</p>").select("p").at(0).set_attr("a b", "v");' 'invalid attribute name'
expect_refusal splice-head 'html.parse("<p>x</p>").select("head").at(0).insert_after("<p>y</p>");' 'select a node inside'
expect_refusal splice-html 'html.parse("<p>x</p>").select("html").at(0).replace_with("<p>y</p>");' 'select a node inside'
ok "mutations: deep fragments, injecting attr names, head/body/html splices refused (exit 3)"

printf '<!doctype html><html><head><meta charset="iso-8859-1"></head><body><p>caf\xe9 &copy;</p></body></html>' > "$WORK/latin1.html"
expect_refusal save-latin1 'html.save(html.load("'"$WORK"'/latin1.html"), "build/latin1-out.html");' 'not valid UTF-8'
[ ! -e build/latin1-out.html ] || fail "non-UTF-8 page was saved with mixed encodings"
ok "Html.save refuses non-UTF-8 sources instead of mixing encodings (exit 3)"

# str_match: escapes, literal unterminated '[', byte semantics, bounded cost.
compile_chunk glob-table 'let cases = 0;
if (str_match("a[c", "a[c")) { cases = cases + 1; }
if (str_match("a[c", "*[c")) { cases = cases + 1; }
if (str_match("a*c", "a\\*c")) { cases = cases + 1; }
if (!str_match("abc", "a\\*c")) { cases = cases + 1; }
if (str_match("a]c", "a[]]c")) { cases = cases + 1; }
if (!str_match("abc", "[z-a]bc")) { cases = cases + 1; }
if (str_match("blog/x/y.html", "blog/*")) { cases = cases + 1; }
if (str_match("", "*") && !str_match("", "?")) { cases = cases + 1; }
report.add("glob", "", cases, ""); report.write_text("'"$WORK"'/glob.txt");'
"$HOST" --dsl "$WORK/glob-table.dslb" >/dev/null || fail "glob table run"
[ "$(cat "$WORK/glob.txt")" = "glob :8: " ] || fail "glob semantics: $(cat "$WORK/glob.txt")"
compile_chunk glob-costly 'let t = "a"; for (i in range(0, 20)) { t = t + t; } let p = "a"; for (i in range(0, 15)) { p = p + p; } str_match(t, "*" + p + "b");'
set +e; out="$("$TIMEOUT_CMD" 10 "$HOST" --dsl "$WORK/glob-costly.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'too costly'* ]] || fail "costly glob not bounded (exit $code): $out"
ok "str_match: escapes, literal '[', empty cases; 1 MiB x 32 KiB worst case refused fast"

compile_chunk link-kinds 'let pages = html.load_dir("tests/fixtures/link-kinds");
for (i in range(0, pages.len())) { let p = pages.at(i); let l = p.links();
  for (j in range(0, l.len())) { report.add(html.link_kind(p, l.at(j)), p.path(), j, ""); } }
report.write_text("'"$WORK"'/link-kinds.txt");'
"$HOST" --dsl "$WORK/link-kinds.dslb" >/dev/null || fail "link-kinds run"
K=tests/fixtures/link-kinds
cat > "$WORK/link-kinds.expected" <<EOF
external $K/based/ext.html:0: 
external $K/index.html:0: 
special $K/index.html:1: 
special $K/index.html:2: 
external $K/index.html:3: 
external $K/index.html:4: 
internal-page $K/index.html:5: 
internal-page $K/index.html:6: 
anchor $K/index.html:7: 
outside-root $K/index.html:8: 
special $K/index.html:9: 
internal-asset $K/index.html:10: 
EOF
diff -u "$WORK/link-kinds.expected" "$WORK/link-kinds.txt" || fail "link_kind classification"
ok "link_kind: uppercase scheme, tab/newline in scheme, backslash hosts, .HTML, base href"

# load_dir patterns: ./ prefix, absolute-below-dir, **/ zero dirs, escapes refused.
G="$WORK/globsite"; mkdir -p "$G/blog/2024"
for f in index.html blog/post.html blog/2024/old.html; do echo '<p>x</p>' > "$G/$f"; done
compile_chunk load-dir-globs 'let n = 0;
n = n + html.load_dir("'"$G"'", "./blog/*", "").len();
n = n + html.load_dir("'"$G"'", "'"$G"'/blog/*", "blog/2024/*").len() * 10;
n = n + html.load_dir("'"$G"'", "**/index.html", "").len() * 100;
n = n + html.load_dir("'"$G"'", "blog/**/post.html", "").len() * 1000;
report.add("globs", "", n, ""); report.write_text("'"$WORK"'/globs.txt");'
"$HOST" --dsl "$WORK/load-dir-globs.dslb" >/dev/null || fail "load_dir globs run"
[ "$(cat "$WORK/globs.txt")" = "globs :1112: " ] || fail "load_dir glob semantics: $(cat "$WORK/globs.txt")"
expect_refusal load-dir-escape 'html.load_dir("'"$G"'", "../*", "");' 'pattern leaves'
expect_refusal load-dir-abs 'html.load_dir("'"$G"'", "/etc/*", "");' 'pattern is outside'
ok "load_dir globs: ./ and absolute-below-dir accepted, **/ matches zero dirs, escapes refused"

compile_chunk dry-run 'let d = html.load("tests/fixtures/tpl/p1.html"); d.select("footer").at(0).set_attr("data-x", "1"); html.save(d, "build/dry-run-out.html"); report.write_text("'"$WORK"'/dry.txt");'
rm -f build/dry-run-out.html
"$HOST" --dry-run --dsl "$WORK/dry-run.dslb" >/dev/null || fail "dry-run run"
[ ! -f build/dry-run-out.html ] || fail "dry-run wrote a file"
grep -qE '^would-save build/dry-run-out\.html:0: [0-9]+ bytes, new$' "$WORK/dry.txt" || fail "dry-run finding wrong: $(cat "$WORK/dry.txt")"
"$HOST" --dsl "$WORK/dry-run.dslb" >/dev/null || fail "real save after dry-run"
[ -f build/dry-run-out.html ] || fail "real save wrote nothing"
"$HOST" --dry-run --dsl "$WORK/dry-run.dslb" >/dev/null || fail "dry-run rerun"
grep -qE 'bytes, unchanged$' "$WORK/dry.txt" || fail "dry-run after save should report unchanged: $(cat "$WORK/dry.txt")"
ok "dry-run reports would-save and writes nothing; unchanged after real save"

compile_chunk empty-fragment 'let d = html.parse("<p>x</p>"); d.select("p").at(0).replace_with("");'
set +e; out="$("$HOST" --dsl "$WORK/empty-fragment.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'produced no nodes'* ]] || fail "empty fragment refusal (exit $code): $out"
ok "empty replacement fragment refused (exit 3)"

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
  case "$UNAME_S" in
    Darwin)
      # macOS cannot link fully static: only system libraries may remain.
      BAD_LIBS="$(otool -L "$HOST" | tail -n +2 | awk '{print $1}' | grep -v '^/usr/lib/\|^/System/Library/' || true)"
      [ -z "$BAD_LIBS" ] || fail "host links non-system libraries: $BAD_LIBS"
      ok "host links only system libraries ($(stat_size "$HOST") bytes, otool -L clean)"
      ;;
    MINGW*|MSYS*|CYGWIN*)
      ok "host built ($(stat_size "$HOST") bytes; static assertion skipped on windows)"
      ;;
    *)
      LDD_OUT="$(ldd "$HOST" 2>&1 || true)"
      [[ "$LDD_OUT" == *'not a dynamic executable'* ]] || fail "host is not fully static: $LDD_OUT"
      ok "host is fully static ($(stat_size "$HOST") bytes, ldd clean)"
      ;;
  esac
fi

echo "PASS: $pass checks"
