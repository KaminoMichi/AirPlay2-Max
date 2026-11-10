#!/usr/bin/env python3
from pathlib import Path
import sys,tempfile,tarfile,json,hashlib,io
R=Path(__file__).resolve().parents[1];sys.path.insert(0,str(R));from tools.airplayctl import validate_bundle,Refusal,root_metadata
cases=[]
for mode in ['valid','absolute','traversal','symlink','duplicate','bad_checksum','bad_publisher','noncanonical','unknown_member']:
 with tempfile.TemporaryDirectory(dir=R.parent/'private',prefix='bundle-') as td:
  d=Path(td);(d/'manage.sh').write_text('fixed script');(d/'ya-publish').write_text('fixed binary fixture')
  sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();files={}
  with tarfile.open(d/'payload.tar','w') as tar:
   names=['PRODUCT']
   if mode=='absolute':names=['/absolute']
   if mode=='traversal':names=['../escape']
   if mode=='noncanonical':names=['a//b']
   if mode=='duplicate':names=['PRODUCT','PRODUCT']
   for name in names:
    i=tarfile.TarInfo(name);body=b'fixed fixture';i.size=len(body)
    if mode=='symlink':i.type=tarfile.SYMTYPE;i.linkname='/not-owned';i.size=0
    tar.addfile(i,io.BytesIO(body));files[name]=hashlib.sha256(body).hexdigest()
  meta={'version':'0.1.0-rc.1','payload_sha256':sha(d/'payload.tar'),'manage_sha256':sha(d/'manage.sh'),'publisher_sha256':sha(d/'ya-publish'),'files':files}
  if mode=='unknown_member':meta['files']={}
  (d/'release.json').write_text(json.dumps(meta))
  if mode=='bad_checksum':(d/'manage.sh').write_text('changed')
  if mode=='bad_publisher':(d/'ya-publish').write_text('changed')
  try:validate_bundle(d)
  except Refusal:assert mode!='valid',mode
  else:assert mode=='valid',mode
  cases.append({'case':mode,'pass':True})
try:root_metadata('exit=0\nexit=1\n')
except Refusal:cases.append({'case':'duplicate root result refused','pass':True})
else:raise AssertionError('duplicate root result')
(R/'build/BUNDLE_TESTS.json').write_text(json.dumps({'pass':True,'platform':'Private archive fixtures; no ADB','cases':cases},indent=2)+'\n');print('PASS archive/result tests',len(cases))
