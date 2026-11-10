#!/usr/bin/env python3
from pathlib import Path
import subprocess,json,os,tempfile,sys
r=Path(__file__).resolve().parents[1];sys.path.insert(0,str(r/'tests/voice'));from lifecycle import lifecycle
proofs=[]
with tempfile.TemporaryDirectory(dir=r.parent/'private',prefix='v-') as td:
 fixture=Path(td);(fixture/'bin').mkdir(mode=0o700);(fixture/'run').mkdir(mode=0o700);fixture.chmod(0o700)
 def run(name,cmd,env=None,timeout=30):
  p=subprocess.run([str(x) for x in cmd],capture_output=True,text=True,env=env,timeout=timeout)
  assert p.returncode==0,(name,p.stdout,p.stderr)
  assert not any(s in p.stderr for s in ['AddressSanitizer','runtime error:','WARNING: ThreadSanitizer']),(name,p.stderr)
  proofs.append({'case':name,'exit':0})
 for name,san in [('asan-ubsan','address,undefined'),('tsan','thread')]:
  binary=fixture/'bin/ya-shairport'
  run(name+'-compile',['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize='+san,'-pthread','-DYR_ROOT="'+str(fixture)+'"','-I'+str(r/'src/runtime'),r/'tests/voice/test-repeat.c','-o',binary])
  env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1','TSAN_OPTIONS':'halt_on_error=1'}
  run(name+'-run',['setarch','x86_64','-R',binary] if name=='tsan' else [binary],env)
  if name=='asan-ubsan':
   # Lifecycle uses a neutral source-root layout and private output only.
   import shutil
   out=r/'out';out.mkdir(exist_ok=True)
   # Driver uses w/runtime for header path; provide source include path alias in commands.
   def lifecycle_run(label,cmd,env=None,timeout=30):
    cmd=[str(x).replace('-I'+str(r/'runtime'),'-I'+str(r/'src/runtime')) for x in cmd];return run(label,cmd,env,timeout)
   reports=lifecycle(r,fixture,lifecycle_run)
   shutil.rmtree(out)
(r/'build/VOICE_TESTS.json').write_text(json.dumps({'pass':True,'platform':'Ubuntu ASan/UBSan/TSan own process/IPC fixtures','checks':proofs,'lifecycle':[{'case':x['case'],'active_removed':x['active_removed']} for x in reports],'android_runtime':'NOT_TESTED new packaging'},indent=2)+'\n');print('PASS voice sanitizer and lifecycle tests')
