#!/usr/bin/env python3
"""Build this one source snapshot; no network or device access."""
from pathlib import Path
import os,json,subprocess,hashlib,shutil,zipfile,argparse
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--deps',type=Path,required=True);p.add_argument('--ndk',type=Path,required=True);p.add_argument('--out',type=Path,default=root/'build');a=p.parse_args()
deps=a.deps.resolve();out=a.out.resolve();tool=a.ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/bin';cc=tool/'aarch64-linux-android28-clang'
assert cc.is_file(),'Android NDK for Linux x86_64 required'
(out/'obj').mkdir(parents=True,exist_ok=True);(out/'payload/bin').mkdir(parents=True,exist_ok=True);(out/'logs').mkdir(exist_ok=True)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
records=[]
def run(label,cmd):
 r=subprocess.run([str(x) for x in cmd],cwd=root,capture_output=True,text=True,env={**os.environ,'SOURCE_DATE_EPOCH':'1794268800','LC_ALL':'C'},timeout=240)
 (out/'logs'/(label+'.txt')).write_text(r.stdout+r.stderr)
 if r.returncode:raise SystemExit(label+' failed; see build log: '+r.stderr[-1800:])
 records.append({'step':label,'exit':0});return r.stdout
stable=['-ffile-prefix-map='+str(root)+'=.','-fdebug-prefix-map='+str(root)+'=.','-fdebug-prefix-map='+str(out)+'=build']
objects=[]
for index,u in enumerate(json.loads((root/'tools/receiver-units.json').read_text())):
 obj=out/'obj'/u['object'];objects.append(obj)
 flags=[x.replace('@DEPS@',str(deps)) for x in u['flags']]
 run('unit-'+str(index),[cc]+flags+stable+['-c',u['source'],'-o',obj])
libs=[deps/x for x in json.loads((root/'tools/library-order.json').read_text())]
assert all(p.is_file() for p in libs),'Missing dependency static archives; use build-deps.py'
run('receiver-link',[cc,'-pthread','-Wl,--no-undefined,--gc-sections,--build-id=sha1','-o',out/'payload/bin/ya-shairport']+objects+['-Wl,--start-group']+libs+['-Wl,--end-group','-lm','-lz','-ldl','-latomic'])
common=[cc,'-std=gnu11','-O2','-g','-Wall','-Wextra','-fPIE','-pie','-Wl,--no-undefined,-z,relro,-z,now,--build-id=sha1']+stable
run('supervisor',common+['src/runtime/supervisor.c','-o',out/'payload/bin/ya-supervisor'])
run('voice',common+['-Isrc/runtime','src/runtime/voice.c','-o',out/'payload/bin/ya-voice'])
run('publish',common+['src/runtime/publish.c','-o',out/'payload/bin/ya-publish'])
run('boot-state',common+['src/runtime/boot-state.c','-o',out/'payload/bin/ya-boot-state'])
run('remote-cli',common+['-Isrc/runtime','src/runtime/remote-cli.c','-o',out/'payload/bin/ya-remote'])
nq=[root/'src/nqptp'/n for n in ['nqptp.c','nqptp-clock-sources.c','nqptp-message-handlers.c','nqptp-utilities.c','general-utilities.c','debug.c']]
nq_objects=[]
for index,source in enumerate(nq+[root/'src/compat/ya-shm.c']):
 obj=out/'obj'/('nq-'+str(index)+'.o');nq_objects.append(obj)
 run('nq-unit-'+str(index),[cc,'-std=gnu11','-O2','-g','-Isrc/nqptp','-Isrc/compat','-Isrc/runtime/volume-base','-include','linux/if_packet.h','-include','src/runtime/volume-base/ya-thread.h','-include','src/compat/ya-shm.h']+stable+['-c',source,'-o',obj])
# Reuse the real cooperative implementation, never declaration-only/no-op cancellation.
spec=json.loads((root/'tools/receiver-units.json').read_text())
thread_objs=[out/'obj'/u['object'] for u in spec if Path(u['source']).name in ['ya-thread.c','ya-stop.c']]
assert len(thread_objs)==2
run('nqptp',common+nq_objects+thread_objs+['-pthread','-o',out/'payload/bin/ya-nqptp'])
for f in (out/'payload/bin').iterdir():
 run('strip-'+f.name,[tool/'llvm-strip','--strip-debug',f]);elf=run('elf-'+f.name,[tool/'llvm-readelf','-h','-d',f]);assert 'AArch64' in elf
 undef=run('imports-'+f.name,[tool/'llvm-nm','-u',f]);assert not any(x in undef for x in [' pthread_cancel',' pthread_testcancel',' pthread_setcancelstate'])
# Record relative source identity, never lab addresses/absolute paths.
result={'version':'0.1.0-rc.1','pass':True,'android_api':28,'arch':'arm64-v8a','runtime_tested':False,'binaries':{str(p.relative_to(out/'payload')):sha(p) for p in (out/'payload/bin').iterdir()},'source_sha256':{str(p.relative_to(root)):sha(p) for p in (root/'src').rglob('*') if p.is_file()},'steps':records}
(out/'BUILD_RESULT.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({'pass':True,'binaries':result['binaries']},indent=2))
