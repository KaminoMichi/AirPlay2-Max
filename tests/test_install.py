#!/usr/bin/env python3
"""Execute device transaction scripts against fake private /data, pm and services."""
from pathlib import Path
import tempfile,subprocess,hashlib,tarfile,os,json,shutil
R=Path(__file__).resolve().parents[1];reports=[]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
for case in ['fresh','same_version','foreign_root','foreign_apk','incompatible','other_receiver','upgrade','rollback','start_failure','uninstall','unsupported_boot','foreign_hook']:
 with tempfile.TemporaryDirectory(dir=R.parent/'private',prefix='install-') as td:
  d=Path(td);root=d/'root';prev=d/'previous';failed=d/'failed';stage=d/'stage-1';stage.mkdir();hook=d/'service.d/ya-airplay.sh';tools=d/'tools';tools.mkdir();state=d/'pm-state';state.mkdir();trace=d/'trace';env=os.environ.copy();env.update(PATH=str(tools)+':'+env['PATH'],YA_FIXTURE=str(d))
  def executable(p,s):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(s);p.chmod(0o700)
  executable(tools/'getprop','''#!/bin/sh
case "$1" in
ro.product.device) echo yandexstation_2;;
ro.build.version.incremental) echo "${MOCK_BUILD:-5000000921}";;
ro.build.version.sdk) echo 28;;
ro.product.cpu.abi) echo arm64-v8a;;
esac
''')
  executable(tools/'getenforce','#!/bin/sh\necho Permissive\n')
  executable(tools/'pidof','#!/bin/sh\n[ "${MOCK_OTHER_RECEIVER:-0}" = 1 ]\n')
  executable(tools/'pm','''#!/bin/sh
s="$YA_FIXTURE/pm-state"
case "$1" in
path) [ ! -f "$s/apk" ] || printf 'package:%s/apk\\n' "$s";;
install) cp "$2" "$s/apk"; echo install >> "$YA_FIXTURE/trace";;
uninstall) rm "$s/apk"; echo uninstall >> "$YA_FIXTURE/trace";;
*) exit 64;; esac
''')
  payload=d/'payload';(payload/'bin').mkdir(parents=True);(payload/'lib').mkdir();(payload/'etc').mkdir();(payload/'PRODUCT').write_text('YA_AIRPLAY_PUBLIC_V1\n');(payload/'VERSION').write_text('0.1.0-rc.1\n');(payload/'lib/nsd.apk').write_text('own-apk');(payload/'etc/shairport.conf.in').write_text('new config')
  executable(payload/'start.sh','#!/bin/sh\necho start >> "$YA_FIXTURE/trace"\n[ "${MOCK_START_FAIL:-0}" != 1 ]\n')
  executable(payload/'stop.sh','#!/bin/sh\necho stop >> "$YA_FIXTURE/trace"\n')
  executable(payload/'bin/ya-supervisor','#!/bin/sh\n[ "$1" = status ]\n')
  (payload/'MANIFEST.sha256').write_text(''.join(sha(p)+'  '+str(p.relative_to(payload))+'\n' for p in payload.rglob('*') if p.is_file()))
  with tarfile.open(stage/'payload.tar','w') as tar:
   for p in payload.rglob('*'):
    if p.is_file():tar.add(p,arcname=str(p.relative_to(payload)))
  text=(R/'device/manage.sh').read_text()
  # The production path strings are replaced only in this owned test copy.
  for a,b in [('/data/local/tmp/ya-airplay.previous',str(prev)),('/data/local/tmp/ya-airplay.failed',str(failed)),('/data/local/tmp/ya-airplay-stage-',str(d/'stage-')),('/data/local/tmp/ya-airplay',str(root)),('/data/adb/service.d/ya-airplay.sh',str(hook)),('/data/adb/service.d',str(hook.parent)),('/data/adb',str(d/'adb'))]:text=text.replace(a,b)
  script=stage/'manage.sh';script.write_text(text)
  op='install';autostart='off';expected=0
  if case in ['same_version','upgrade','rollback','uninstall']:
   shutil.copytree(payload,root);root.chmod(0o700);(root/'run').mkdir(mode=0o700)
   if case in ['upgrade','rollback']:(root/'VERSION').write_text('0.0.9\n')
   if case=='uninstall':(root/'APK_CREATED').write_text('ours');(root/'APK_SHA256').write_text(sha(payload/'lib/nsd.apk')+'\n')
  if case=='foreign_root':root.mkdir();(root/'keep').write_text('foreign');expected=65
  if case=='foreign_apk':(state/'apk').write_text('foreign-apk');expected=65
  if case=='incompatible':env['MOCK_BUILD']='unexpected';expected=65
  if case=='other_receiver':env['MOCK_OTHER_RECEIVER']='1';expected=75
  if case=='unsupported_boot':autostart='required';expected=69
  if case=='foreign_hook':
   hook.parent.mkdir(mode=0o700);hook.write_text('foreign');hook.chmod(0o600);autostart='required';expected=65
  if case=='upgrade':op='update';(root/'etc/shairport.conf.in').write_text('owner config')
  if case=='rollback':
   op='rollback';shutil.copytree(payload,prev);prev.chmod(0o700);(prev/'VERSION').write_text('0.0.8\n')
  if case=='start_failure':env['MOCK_START_FAIL']='1';expected=1
  if case=='uninstall':op='uninstall';shutil.copy2(payload/'lib/nsd.apk',state/'apk')
  for base in [root,prev]:
   if base.exists():
    for p in base.rglob('*'):
     if p.is_file() and not os.access(p,os.X_OK):p.chmod(0o600)
  result=subprocess.run(['sh',str(script),op,autostart],capture_output=True,text=True,env=env,timeout=10)
  assert result.returncode==expected,(case,result.returncode,result.stdout,result.stderr)
  t=trace.read_text().splitlines() if trace.exists() else []
  if case=='fresh':assert root.exists() and (root/'APK_CREATED').exists() and t==['install','start']
  if case=='same_version':assert not t and not prev.exists()
  if case=='foreign_root':assert (root/'keep').read_text()=='foreign' and not t
  if case=='foreign_apk':assert (state/'apk').read_text()=='foreign-apk' and not t
  if case in ['incompatible','other_receiver','unsupported_boot','foreign_hook']:assert not root.exists() and not t
  if case=='upgrade':assert prev.exists() and (root/'etc/shairport.conf.in').read_text()=='owner config' and t==['install','stop','start']
  if case=='rollback':assert (root/'VERSION').read_text()=='0.0.8\n' and failed.exists() and t==['stop']
  if case=='start_failure':assert not root.exists() and failed.exists() and not (state/'apk').exists() and t==['install','start','stop','uninstall']
  if case=='uninstall':assert not root.exists() and failed.exists() and not (state/'apk').exists() and t==['stop','uninstall']
  reports.append({'case':case,'exit':result.returncode,'pass':True})
(R/'build/INSTALL_TESTS.json').write_text(json.dumps({'pass':True,'platform':'Ubuntu private filesystem and pm/service doubles; not Android','cases':reports},indent=2)+'\n');print('PASS installer transaction cases',len(reports))
