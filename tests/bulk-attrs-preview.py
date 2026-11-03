#!/usr/bin/env python3
"""Exercise real run_lmcode preview frames, not direct native callbacks or exit 0 alone."""
from pathlib import Path
import json, os, subprocess, sys, tempfile
root=Path(__file__).resolve().parents[1]
owner=root.name if root.name in ("lmhtml","lmdraw","lmvideo") else 'lmhtml'
host=Path(sys.argv[1]).resolve();compiler=sys.argv[2]
if os.name=="nt":compiler=subprocess.check_output(["bash","-c",'cygpath -u "$1"',"bulk-preview",compiler],encoding="utf-8").strip()
with tempfile.TemporaryDirectory(prefix="bulk-preview-") as temp:
 work=Path(temp);(work/"build/rendered").mkdir(parents=True)
 schema=root/"schema"/(owner+(".host.json" if owner=="lmhtml" else ".dsl.json"))
 cases=[('bulk-preserves-concrete-node', 'let doc=html.parse("<div title=\'old\'></div>");let node=doc.select("div").at(0);node.set_attrs(%(title=>"new"));$return.unchanged=node.attr("title")=="old";html.save(doc,"build/dry.html");', {'unchanged': True}), ('bulk-getter-is-passive', 'let doc=html.parse("<div title=\'old\' data-empty=\'\'></div>");let node=doc.select("div").at(0);let got=node.get_attrs(qw(title data-empty absent));got.set("title","copy");$return.check=got.len()==2 && got.has("data-empty") && !got.has("absent") && node.attr("title")=="old";', {'check': True})]
 for label,body,expected in cases:
  source="p=_lmcode_ schema="+json.dumps(str(schema))+";\n"+body+"\nend _lmcode_\nr=run_lmcode(p,{command:"+json.dumps([str(host),"--lmcode","$DSLFILE"])+"});\nprint(json_stringify(r));\n"
  file=work/(label+".lms");file.write_text(source,encoding="utf-8")
  result=subprocess.run(["bash","-c",'exec "$@"',"bulk-preview",compiler,"--dry-run",str(file)],cwd=work,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,encoding="utf-8")
  assert result.returncode==0,(label,result.stdout)
  frames=[]
  for line in result.stdout.splitlines():
   if 'dry host run (simulated)' in line and '{"argv"' in line:frames.append(json.loads(line[line.index('{"argv"'):])["envelope"])
  assert len(frames)==1,(label,result.stdout)
  frame=frames[0];assert frame.get("dry") is True and frame.get("ok") is True,(label,frame,result.stdout)
  if expected is not None:assert frame.get("result")==expected,(label,frame,result.stdout)
  assert not list((work/"build").rglob("*.png")) and not list((work/"build").rglob("*.html")),(label,"dry wrote a product file")
  print("PASS real preview:",label)
