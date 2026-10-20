#!/usr/bin/env python3
import json, subprocess, sys, tempfile, threading, shutil
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
host=str(Path(sys.argv[1]).resolve()); count=0

def write(path, text):
 path.write_bytes(text.encode("utf-8"))

def run(*args,ok=True):
 global count
 r=subprocess.run([host,*map(str,args)],capture_output=True,text=True,encoding="utf-8")
 assert (r.returncode==0)==ok,(args,r.returncode,r.stdout,r.stderr)
 count+=1
 return r
with tempfile.TemporaryDirectory(dir='build',prefix='plain-') as tmp:
 p=Path(tmp); page=p/'page.html'
 write(page, '''<!doctype html><html><head><title>Plain</title></head><body><main><h1>Plain</h1><p class="x" style="color:red">A <strong>bold</strong> <em>term</em> <code>room</code> &quot;quote&quot; &#39;one&#39; &gt; two &mdash; &nbsp; &copy;\nnext.</p><pre><code>echo &quot;x&quot; &gt; file\n  echo &lt;x> &amp; y</code></pre><p><a href="a?x=1&amp;y=&quot;q&quot;" title="a&apos;b">link</a></p></main></body></html>\n''')
 before=json.loads(run('lint','--stats','--json',page,ok=False).stdout)['files'][-1]
 assert before['counts']['strong']==1 and before['counts']['inline_code']==1 and before['counts']['entity:&quot;']==4
 assert before['words']>0 and before['per_100_words']['strong']>0
 run('normalize',page); out=page.read_text(encoding="utf-8")
 assert '<strong>' not in out and '<em>' not in out and '<code>room' not in out
 assert '"quote"' in out and "'one' > two —   ©" in out
 assert 'echo "x" > file\n  echo &lt;x> &amp; y' in out
 assert 'href="a?x=1&amp;y=&quot;q&quot;"' in out and 'title="a\'b"' in out
 run('normalize','--check',page);run('lint',page)
 run('clean','--check',page)
 original=p/'before';original.mkdir();write(original/'a.html', '<p><b>old</b></p>')
 delta=json.loads(run('lint','--json','--compare',original,page).stdout)['delta'];assert delta['counts']['b']==-1
 policy=p/'policy';write(policy, str(original)+' b 1\n')
 run('lint','--config',policy,original)
 dirty=p/'dirty.html';write(dirty, '<main><h1>x</h1><nav>menu</nav><div class="cookie-banner">accept</div><p><span>Hello</span> <a href="../b#x">world</a></p><img src="/a.png" alt="diagram"><script>bad()</script></main>')
 clean=run('clean',dirty,'--base','https://example.org/docs/a.html').stdout
 assert 'menu' not in clean and 'accept' not in clean and 'bad()' not in clean
 assert '<p>Hello <a href="https://example.org/b#x">world</a></p>' in clean
 assert '<img src="https://example.org/a.png" alt="diagram">' in clean
 run('clean','--check',dirty,ok=False)
 md=run('clean',dirty,'--markdown').stdout;assert '**' not in md and '`' not in md
 write(dirty, '<p>Chrome</p><div role="main"><h5>Deep heading</h5><ul><li>One</li><ul><li>Nested</li></ul></ul></div>')
 md=run('clean',dirty,'--markdown').stdout;assert 'Chrome' not in md and '##### Deep heading' in md and '  - Nested' in md
 note=p/'note.md';write(note, '# Note\n\nA **bold** and *term* with `path`\non another line.\n\n```sh\necho "x"\n```\n')
 counts=json.loads(run('md-lint','--json',note,ok=False).stdout)['files'][-1]['counts'];assert counts['md_emphasis']==2 and counts['md_inline_code']==1 and counts['hard_wrap']==1
 run('normalize',note);run('normalize','--check',note);run('lint',note)
 assert 'A bold and term with path on another line.' in note.read_text(encoding="utf-8")
 write(note, '# Note\n\n<div>bad</div>\n\nEmoji 😀  \n')
 bad=json.loads(run('lint','--json',note,ok=False).stdout)['files'][-1]['counts'];assert bad['raw_html'] and bad['emoji'] and bad['trailing_whitespace']
 for markup in ['<p></p>','<p id="bad">x</p>','<p a="1" a="2">x</p>','<table><tr><td class="x">one</td></tr></table>']:
  write(page, markup);run('lint',page,ok=False);run('normalize',page);run('lint',page);run('normalize','--check',page)
 write(page, '<main><h1>x</h1><pre><kbd>one</kbd> \n<!-- separator --><samp>two</samp></pre></main>')
 run('normalize',page);run('lint',page);run('clean','--check',page)
 write(page, "<p title='literal'>unchanged"+" &amp; &lt; \" ' > ©</p>\n");run('normalize',page);assert page.read_text(encoding="utf-8")=="<p title='literal'>unchanged &amp; &lt; \" ' > ©</p>\n"
 write(page, '<p>unchanged &amp; &lt; " \' > ©</p>\n');run('normalize',page);assert page.read_text(encoding="utf-8")=='<p>unchanged &amp; &lt; " \' > ©</p>\n'
if shutil.which('curl'):
 class Handler(BaseHTTPRequestHandler):
  def log_message(self,*args): pass
  def do_GET(self):
   if self.path=='/redirect':
    self.send_response(302);self.send_header('Location','/dir/page.html');self.end_headers()
   else:
    self.send_response(200);self.end_headers();self.wfile.write(b'<main><p><a href="next.html">Next</a></p></main>')
 server=HTTPServer(('127.0.0.1',0),Handler)
 thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
 try:
  origin='http://127.0.0.1:'+str(server.server_port)
  extracted=run('clean',origin+'/redirect').stdout
  assert origin+'/dir/next.html' in extracted
 finally:
  server.shutdown();server.server_close();thread.join()
print(f'PASS: {count} plain-profile CLI checks')
