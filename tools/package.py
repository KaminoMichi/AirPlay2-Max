#!/usr/bin/env python3
"""Deterministic release packaging; never installs or publishes."""
from pathlib import Path
import hashlib,json,tarfile,gzip,shutil,io,sys
r=Path(__file__).resolve().parents[1];out=r/'build';stage=out/'payload';dest=r.parent/'artifacts';dest.mkdir(exist_ok=True)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
checks=['BUILD_RESULT','JAVA_RESULT','HOST_TESTS','INSTALL_TESTS','VOICE_TESTS','VOICE_STOP_TESTS','REPRODUCIBILITY','LOG_LIMIT_TEST','BUNDLE_TESTS','RELINK_TEST','DEPENDENCY_BUILD_RESULT']
for name in checks:
 j=json.loads((out/(name+'.json')).read_text());assert j['pass'],name
j=json.loads((out/'BUILD_RESULT.json').read_text())
for name,h in j['binaries'].items():assert sha(stage/name)==h,name
for name,h in j['source_sha256'].items():assert sha(r/name)==h,name
j=json.loads((out/'JAVA_RESULT.json').read_text())
for name,h in j['artifacts'].items():assert sha(stage/name)==h,name
for name,h in j['source_sha256'].items():assert sha(r/name)==h,name
for p in (r/'device').glob('*.sh'):shutil.copy2(p,stage/p.name)
(stage/'etc').mkdir(exist_ok=True);shutil.copy2(r/'config/shairport.conf.in',stage/'etc/shairport.conf.in')
(stage/'PRODUCT').write_text('YA_AIRPLAY_PUBLIC_V1\n');(stage/'VERSION').write_text('0.1.0-rc.1\n')
files=sorted(p for p in stage.rglob('*') if p.is_file() and p.name not in ['MANIFEST.sha256','ya-publish'])
(stage/'MANIFEST.sha256').write_text(''.join(sha(p)+'  '+str(p.relative_to(stage))+'\n' for p in files));files.append(stage/'MANIFEST.sha256')
release=r/'release';release.mkdir(exist_ok=True)
with tarfile.open(release/'payload.tar','w',format=tarfile.USTAR_FORMAT) as tar:
 for p in sorted(files):
  n=str(p.relative_to(stage));info=tarfile.TarInfo(n);info.size=p.stat().st_size;info.mode=0o700 if n.startswith('bin/') or n.endswith('.sh') else 0o600;info.mtime=0
  with p.open('rb') as f:tar.addfile(info,f)
shutil.copy2(r/'device/manage.sh',release/'manage.sh');shutil.copy2(stage/'bin/ya-publish',release/'ya-publish')
meta={'version':'0.1.0-rc.1','prerelease':True,'payload_sha256':sha(release/'payload.tar'),'manage_sha256':sha(release/'manage.sh'),'publisher_sha256':sha(release/'ya-publish'),'files':{str(p.relative_to(stage)):sha(p) for p in sorted(files)},'baseline':'Device-tested SF1 + VP2; owner report 2026-11-10','new_installation_device_tested':False}
(release/'release.json').write_text(json.dumps(meta,indent=2)+'\n')
sys.path.insert(0,str(r));from tools.airplayctl import validate_bundle
assert validate_bundle(release)==meta
# Public validation reports carry only relative source IDs and fixed test cases.
validation=r/'validation';validation.mkdir(exist_ok=True)
for name in checks:shutil.copy2(out/(name+'.json'),validation/(name+'.json'))
license_files=[p for p in (r/'licenses').rglob('*') if p.is_file()]
install_files=[r/'install.py',r/'uninstall.py',r/'tools/airplayctl.py']+list(release.iterdir())+license_files+[r/n for n in ['README.md','README.en.md','INSTALL.md','UNINSTALL.md','TROUBLESHOOTING.md','LICENSE','THIRD_PARTY_NOTICES.md','RELEASE_NOTES.md']]
source_files=[]
for p in r.rglob('*'):
 if not p.is_file():continue
 rel=p.relative_to(r)
 if any(x in ['.git','build','release','out','__pycache__'] or x.startswith('build-') for x in rel.parts[:-1]) or any(x.startswith('._') for x in rel.parts):continue
 if p.suffix=='.pyc':continue
 source_files.append(p)
assert {'tools/build.py','tools/build-java.py','tools/build-deps.py','tools/relink.py'} <= {str(p.relative_to(r)) for p in source_files},'Missing required build tool'
# Complete allowlist for the clean Git tree and source archive.
(r/'SOURCE_SHA256.json').write_text(json.dumps({str(p.relative_to(r)):sha(p) for p in sorted(source_files) if p.name!='SOURCE_SHA256.json'},indent=2)+'\n');source_files=[p for p in source_files if p.name!='SOURCE_SHA256.json']+[r/'SOURCE_SHA256.json']
def archive(name,paths,prefix):
 target=dest/name;assert not target.exists(),'Refuse overwriting sealed release: '+name
 with target.open('wb') as raw,gzip.GzipFile(fileobj=raw,filename='',mode='wb',mtime=0) as gz,tarfile.open(fileobj=gz,mode='w') as tar:
  for p in sorted(paths):
   info=tar.gettarinfo(str(p),arcname=prefix+'/'+str(p.relative_to(r)));info.uid=info.gid=0;info.uname=info.gname='';info.mtime=0
   with p.open('rb') as f:tar.addfile(info,f)
 with tarfile.open(target) as tar:
  for m in tar.getmembers():
   assert m.isfile();rel=m.name.removeprefix(prefix+'/');assert hashlib.sha256(tar.extractfile(m).read()).hexdigest()==sha(r/rel)
 return target
install=archive('ya-airplay-0.1.0-rc.1-install.tar.gz',install_files,'ya-airplay-0.1.0-rc.1')
source=archive('ya-airplay-0.1.0-rc.1-source.tar.gz',source_files,'ya-airplay-0.1.0-rc.1-source')
assets=[install,source]+[dest/j[k]['file'] for k in ['third_party_sources','relink_kit'] for j in [json.loads((r/'tools/dependency-artifacts.json').read_text())]]
for p in assets:assert p.is_file()
(dest/'SHA256SUMS').write_text(''.join(sha(p)+'  '+p.name+'\n' for p in assets))
manifest={'version':'0.1.0-rc.1','prerelease':True,'assets':{p.name:{'sha256':sha(p),'bytes':p.stat().st_size} for p in assets},'github_target':'KaminoMichi/AirPlay2-Max','branch':'main','published':False,'device_changed':False,'new_installation_device_tested':False}
(dest/'DELIVERY.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps(manifest,indent=2))
