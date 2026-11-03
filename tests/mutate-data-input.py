"""Compile baseline and one isolated production-copy dry-to-real mutant."""
import pathlib,shutil,subprocess,tempfile,os
root=pathlib.Path(__file__).resolve().parents[1]
cxx=os.environ.get('CXX') or shutil.which('g++') or shutil.which('clang++') or 'c++'
with tempfile.TemporaryDirectory(prefix='lmhtml-data-mutation-') as d:
    p=pathlib.Path(d);(p/'examples/data-input').mkdir(parents=True);(p/'tests').mkdir()
    original=(root/'examples/data-input/host.cpp').read_text();target=p/'examples/data-input/host.cpp'
    shutil.copyfile(root/'tests/data-input.cpp',p/'tests/data-input.cpp')
    exe=p/('unit.exe' if os.name=='nt' else 'unit')
    def run(text):
        target.write_text(text)
        subprocess.run([cxx,'-std=c++17','-O0','-I',str(root/'cpp'),str(p/'tests/data-input.cpp'),'-o',str(exe)],check=True)
        return subprocess.run([str(exe),str(p/'sentinel.json')],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    r=run(original);assert r.returncode==0,r.stderr;print('baseline PASS')
    old='dslvm::dry(dslvm::Function([&registry,&counts](dslvm::Args&a){++counts.preview;validate(registry,a);return dslvm::Value::boolean(true);} ))'
    old=old.replace(';} ))',';}))')
    assert original.count(old)==1,'mutation anchor'
    r=run(original.replace(old,'dslvm::dry(real)'));assert r.returncode!=0;assert b'dry invoked real callback' in r.stderr,r.stderr
    print('CAUGHT dry-to-real fallback: real callback counter/sentinel assertion')
