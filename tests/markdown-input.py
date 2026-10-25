#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
host = str(Path(sys.argv[1]).resolve())
compiler = sys.argv[2]
root = Path.cwd()
fixtures = root / 'tests/fixtures/markdown'
count = 0
def run(*args, ok=True):
    global count
    p = subprocess.run([host, *map(str,args)], capture_output=True)
    assert (p.returncode == 0) == ok, (args, p.returncode, p.stdout, p.stderr)
    count += 1
    return p.stdout if ok else p.stderr
with tempfile.TemporaryDirectory(dir=root/'build', prefix='markdown-') as temp:
    work=Path(temp); notes=work/'notes'; notes.mkdir()
    for name in ('features.md','other.md','pixel.svg'): shutil.copy(fixtures/name,notes/name)
    rendered=run('md2html',notes/'features.md')
    assert rendered == (fixtures/'features.golden.html').read_bytes()
    assert b'<script>' not in rendered and b'&lt;script>' in rendered
    assert b'class=' not in rendered and b'<code>table code</code>' in rendered
    for part in (b'id="hello-world"',b'id="hello-world-2"',b'<table>',b'<del>old</del>',b'<ul>',b'data-language="sh"',b'<br>',b'type="checkbox"'):
        assert part in rendered, part
    assert run('md2html',notes/'features.md') == rendered
    assert b'<script>' in run('md2html',notes/'features.md','--allow-html')
    templated=run('md2html',notes/'features.md','--template',fixtures/'template.html')
    assert b'&lt;title>' in templated and b'{{content}}' in templated
    assert b'<nav><ul><li><a href="#hello-world">' in templated
    assert b'unknown' in run('md2html',notes/'other.md','--unknown',ok=False)
    run('index',notes)
    original=(notes/'index.md').read_bytes(); stamp=(notes/'index.md').stat().st_mtime_ns
    run('index',notes); assert (notes/'index.md').stat().st_mtime_ns==stamp
    run('index',notes,'--check'); assert b'features.md' in original and b'&' in original
    (work/'order.txt').write_text('other.md\nfeatures.md\n')
    run('index',notes,'--order',work/'order.txt')
    assert (notes/'index.md').read_text().index('other.md') < (notes/'index.md').read_text().index('features.md')
    assert b'index drift' in run('index',notes,'--check',ok=False)
    run('index',notes)
    run('mdcheck',notes)
    out=work/'web';run('site',notes,out,'--template',fixtures/'template.html')
    assert b'other.html#details' in (out/'features.html').read_bytes()
    assert (out/'pixel.svg').read_bytes()==(notes/'pixel.svg').read_bytes()
    first={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir()}
    run('site',notes,out,'--template',fixtures/'template.html')
    assert first=={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir()}
    (notes/'features.html').write_text('<p>colliding asset</p>')
    assert b'collides' in run('site',notes,work/'collision',ok=False)
    assert not (work/'collision').exists()
    (notes/'features.html').unlink()
    assert b'overlap' in run('site',notes,notes/'out',ok=False)
    assert b'outside' in run('md2html',notes/'features.md',root.parent/'outside.html',ok=False)
    (notes/'index.md').unlink();run('site',notes,work/'no-index')
    assert (work/'no-index/index.html').exists() and not (notes/'index.md').exists()
    (notes/'bad.md').write_text('# Bad\n\n[broken](missing.md#x)')
    assert b'broken link' in run('mdcheck',notes,ok=False)
    assert b'broken link' in run('site',notes,work/'bad-out',ok=False)
    assert not (work/'bad-out').exists()
    (notes/'bad.md').write_text('---\ntitle: unterminated')
    assert b'unterminated' in run('md2html',notes/'bad.md',ok=False)
    (notes/'bad.md').unlink()
    source='''p = _lmcode_ cli="lmhtml" install="github:lmctl-ai/lmhtml@latest";
let doc = html.from_markdown("# Heading\\n\\nSummary.\\n\\n## Child");
if (doc.title() != "Heading" || doc.summary() != "Summary.") { report.add("bad", "metadata", 0, "wrong metadata"); }
if (html.load_dir(PATH, "*.md", "").len() != 2) { report.add("bad", "load", 0, "missing Markdown"); }
let rendered = html.render_markdown("# Safe <title>", "<!doctype html><html><head><title>{{title}}</title></head><body><main>{{content}}</main></body></html>", "note.md");
rendered.select("h1").at(0).set_attr("id", "post-processed");
html.save(rendered, OUTPUT);
html.check_markdown(PATH);
let index = html.index_markdown(PATH, "other.md\\nfeatures.md");
report.fail_if_any();
end _lmcode_
savedsl(p, ARGV[0]);
'''.replace('PATH',json.dumps(notes.as_posix())).replace('OUTPUT',json.dumps((work/'dsl.html').as_posix()))
    (work/'test.lms').write_text(source)
    command=[compiler,str(work/'test.lms'),str(work/'test.dslb')]
    # npm's extensionless launcher is a shell script on Windows, not a PE file.
    if os.name == 'nt': command=[sys.argv[3], '-c', 'exec "$@"', 'lmhtml-test', *command]
    subprocess.run(command,check=True)
    run('--dsl',work/'test.dslb')
    assert b'post-processed' in (work/'dsl.html').read_bytes()
print(f'PASS: {count} Markdown input CLI/DSL checks')
