#!/usr/bin/env python3
"""Only private Linux fixtures; never ADB, real processes or root queue."""
from pathlib import Path
import subprocess,tempfile,os,sys,json,shutil,fcntl,hashlib,tarfile,io
sys.path.insert(0,str(Path(__file__).resolve().parents[1]));from tools.airplayctl import compatibility,Refusal,FINGERPRINT,validate_bundle
R=Path(__file__).resolve().parents[1];W=R.parent/'private';W.mkdir(exist_ok=True);results=[]
def record(name):results.append({'case':name,'pass':True})
def command(args,expected=0,env=None):
 p=subprocess.run([str(x) for x in args],capture_output=True,text=True,env=env,timeout=15)
 assert p.returncode==expected,(args,p.returncode,p.stdout,p.stderr)
 assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr and 'WARNING: ThreadSanitizer' not in p.stderr,(args,p.stderr)
 return p
props={'ro.product.device':'yandexstation_2','ro.build.version.incremental':'5000000921','ro.build.version.sdk':'28','ro.product.cpu.abi':'arm64-v8a','ro.build.fingerprint':FINGERPRINT,'selinux':'Permissive'}
compatibility(props,300000);record('compatible properties')
for key in props:
 q=dict(props);q[key]='unexpected'
 try:compatibility(q,300000)
 except Refusal:record('reject incompatible '+key)
 else:raise AssertionError(key)
try:compatibility(props,1)
except Refusal:record('reject insufficient free space')
else:raise AssertionError('disk gate')
# Boot state: real file locks, root-only owned fixtures, no PID signaling.
with tempfile.TemporaryDirectory(dir=W,prefix='boot-') as td:
 d=Path(td);root=d/'r';root.mkdir(mode=0o700);run=root/'run';run.mkdir(mode=0o700)
 def put(p,s):p.write_text(s);p.chmod(0o600)
 put(root/'PRODUCT','YA_AIRPLAY_PUBLIC_V1\n');boot=d/'boot';put(boot,'11111111-1111-1111-1111-111111111111\n')
 binary=d/'boot-state';command(['gcc','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-DYAB_ROOT="'+str(root)+'"','-DYAB_BOOT_ID="'+str(boot)+'"',R/'src/runtime/boot-state.c','-o',binary])
 command([binary,'prepare']);assert (run/'boot-id').read_bytes()==boot.read_bytes();record('boot first initialization')
 put(run/'active','current');command([binary,'prepare']);assert (run/'active').read_text()=='current';record('same boot preserves active state')
 put(boot,'22222222-2222-2222-2222-222222222222\n')
 with (run/'supervisor.lock').open('w') as lock:
  os.chmod(lock.name,0o600);fcntl.flock(lock,fcntl.LOCK_EX)
  command([binary,'prepare'],69);assert (run/'active').exists();record('live supervisor lock blocks cold reset')
 foreign=d/'foreign';put(foreign,'preserve');(run/'shm-nqptp').symlink_to(foreign)
 command([binary,'prepare'],69);assert foreign.read_text()=='preserve' and (run/'active').exists();record('cold reset rejects symlink before unlink')
 (run/'shm-nqptp').unlink();os.link(foreign,run/'shm-nqptp');command([binary,'prepare'],69);assert (run/'active').exists();record('cold reset rejects hardlink')
 (run/'shm-nqptp').unlink();command([binary,'prepare']);assert not (run/'active').exists();assert (run/'boot-id').read_bytes()==boot.read_bytes();record('cold boot resets only known stale objects')
 put(run/'active','must preserve');put(boot,'invalid');command([binary,'prepare'],69);assert (run/'active').exists();record('invalid boot identity refuses reset')
# Atomic publication uses the same renameat2 syscall, with fixture-only paths.
with tempfile.TemporaryDirectory(dir=W,prefix='pub-') as td:
 d=Path(td);prefix=str(d/'r-');target=d/'queue';binary=d/'publisher'
 command(['gcc','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-DYAP_PREFIX="'+prefix+'"','-DYAP_TARGET="'+str(target)+'"',R/'src/runtime/publish.c','-o',binary])
 sources=[]
 for i in range(2):
  parent=Path(prefix+f'{i:016x}');parent.mkdir(mode=0o700);p=parent/'request.part';p.write_text('fixed request\n');p.chmod(0o600);sources.append(p)
 first=command([binary,sources[0]]);assert target.read_text()=='fixed request\n';record('atomic single-link publication')
 before=target.stat().st_ino;command([binary,sources[1]],75);assert sources[1].exists() and target.stat().st_ino==before;record('existing request never overwritten')
 target.unlink();foreign=d/'foreign';foreign.write_text('keep');sources[1].unlink();sources[1].symlink_to(foreign);command([binary,sources[1]],65);assert foreign.read_text()=='keep' and not target.exists();record('publisher rejects symlink')
# Log retention fixtures exercise actual shell code and preserve unknown files.
for mode in ['normal','hidden','symlink','hardlink']:
 with tempfile.TemporaryDirectory(dir=W,prefix='logs-') as td:
  d=Path(td);run=d/'run';run.mkdir();dirs=[]
  for i in range(6):
   p=run/f'log-179431000{i}-100';p.mkdir(mode=0o700);f=p/'receiver.txt';f.write_text('own log');f.chmod(0o600);dirs.append(p)
  if mode=='hidden':(dirs[0]/'.foreign').write_text('preserve')
  if mode in ['symlink','hardlink']:
   foreign=d/'foreign';foreign.write_text('preserve');(dirs[0]/'receiver.txt').unlink()
   if mode=='symlink':(dirs[0]/'receiver.txt').symlink_to(foreign)
   else:os.link(foreign,dirs[0]/'receiver.txt')
  script=d/'prune.sh';script.write_text((R/'device/prune-logs.sh').read_text().replace('/data/local/tmp/ya-airplay/run',str(run)))
  command(['sh',script],0 if mode=='normal' else 65)
  assert len(list(run.iterdir()))==(4 if mode=='normal' else 6)
  if mode in ['symlink','hardlink']:assert foreign.read_text()=='preserve'
  record('log retention '+mode)
for p in (R/'device').glob('*.sh'):command(['sh','-n',p])
record('all device shell syntax')
(R/'build/HOST_TESTS.json').write_text(json.dumps({'pass':True,'platform':'Ubuntu private fixtures; no device actions','cases':results},indent=2)+'\n');print('PASS host safety tests',len(results))
