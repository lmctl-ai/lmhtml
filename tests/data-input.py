"""Offline CLI/manifest integration. Explicit compiler path until with= is published."""
import json, pathlib, subprocess, sys, tempfile
host,compiler=map(lambda p:str(pathlib.Path(p).resolve()),sys.argv[1:3])
source=(pathlib.Path(__file__).resolve().parents[1]/'examples/data-input/example.lms').read_text()
with tempfile.TemporaryDirectory(prefix='lmhtml-data-input-') as d:
    root=pathlib.Path(d);(root/'schema.json').write_bytes(subprocess.check_output([host,'--lmcode-schema','--json']))
    target=root/'accepted.json'
    def run(text, mode):
        f=root/'example.lms';f.write_text(text,encoding='utf-8')
        cmd=['node',compiler]+([mode] if mode else [])+[str(f),host,str(target)]
        r=subprocess.run(cmd,cwd=root,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        assert r.returncode==0,(r.stdout.decode(errors='replace'),r.stderr.decode(errors='replace'))
        return r.stdout.decode(),r.stderr.decode()
    out,err=run(source,'--check');assert out==err=='';assert not target.exists()
    out,err=run(source,'--dry-run');assert 'preview=1 real=0' in (out+err) and not target.exists(),(out,err)
    out,err=run(source,'');assert 'preview=0 real=1' in err;(data:=json.loads(target.read_text()));assert data=={'title':'water 水','data-note':'quoted " text'};target.unlink()
    for bad in [source.replace('"json", ${input.toString()}','"toml", ${input.toString()}'),source.replace('"title":"water 水"','"title":7')]:
        out,err=run(bad,'--dry-run');assert 'preview=1 real=0' in (out+err) and not target.exists(),(out,err);assert '"ok":false' in out,(out,err)
print('PASS actual lmscript: check dormant, dry validation only, real once, invalid format/domain refused')
