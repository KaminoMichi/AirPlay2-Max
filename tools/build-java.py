#!/usr/bin/env python3
from pathlib import Path
import argparse,subprocess,zipfile,hashlib,shutil,json
p=argparse.ArgumentParser();p.add_argument('--sdk',type=Path,required=True);p.add_argument('--signed-nsd',type=Path,required=True);p.add_argument('--reference-volume',type=Path,required=True);a=p.parse_args();root=Path(__file__).resolve().parents[1];out=root/'build';bt=a.sdk/'build-tools/30.0.3';jar=a.sdk/'platforms/android-23/android.jar';steps=[]
def run(cmd):
 r=subprocess.run([str(x) for x in cmd],capture_output=True,text=True,timeout=90)
 if r.returncode:raise SystemExit(r.stderr+r.stdout)
 steps.append({'tool':Path(cmd[0]).name,'exit':0})
for name in ['nsd','volume']:
 classes=out/('java-'+name);classes.mkdir(parents=True,exist_ok=True)
 sources=sorted((root/'android'/name).rglob('*.java'))
 run(['javac','--release','8','-classpath',jar,'-d',classes]+sources)
 dex=out/(name+'.dex');run([bt/'dx','--dex','--output='+str(dex),classes])
 if name=='volume':
  assert hashlib.sha256(a.reference_volume.read_bytes()).hexdigest()=='df0acdfd7f18ed412231a18b31442024aacac8f52f45b98096236ad7e1b98cde','Wrong baseline volume artifact'
  with zipfile.ZipFile(a.reference_volume) as known:
   assert known.read('classes.dex')==dex.read_bytes(),'Volume source/DEX mismatch'
  shutil.copy2(a.reference_volume,out/'payload/lib/volume-bridge.jar')
 else:
  unsigned=out/'nsd-unsigned.apk';run([bt/'aapt','package','-f','-M',root/'android/nsd/AndroidManifest.xml','-I',jar,'-F',unsigned])
  with zipfile.ZipFile(unsigned,'a') as z:z.writestr('classes.dex',dex.read_bytes())
  run([bt/'apksigner','verify',a.signed_nsd])
  with zipfile.ZipFile(a.signed_nsd) as known,zipfile.ZipFile(unsigned) as built:
   for entry in ['classes.dex','AndroidManifest.xml']:
    assert known.read(entry)==built.read(entry),'NSD source/binary mismatch: '+entry
  shutil.copy2(a.signed_nsd,out/'payload/lib/nsd.apk')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
j={'pass':True,'nsd_apk_signature_preserved':True,'rebuilt_nsd_dex_and_manifest_exact':True,'rebuilt_volume_dex_exact_and_jar_preserved':True,'runtime_tested':False,'artifacts':{str(p.relative_to(out/'payload')):sha(p) for p in (out/'payload/lib').iterdir()},'source_sha256':{str(p.relative_to(root)):sha(p) for p in (root/'android').rglob('*') if p.is_file()},'steps':steps}
(out/'JAVA_RESULT.json').write_text(json.dumps(j,indent=2)+'\n');print(json.dumps({k:v for k,v in j.items() if k!='source_sha256'},indent=2))
