#!/usr/bin/env python3
"""Offline rebuild of the pinned dependency source archive, in a new directory."""
from pathlib import Path
import argparse,subprocess,os,json,tarfile,hashlib,shutil
p=argparse.ArgumentParser();p.add_argument('--resume',action='store_true');p.add_argument('--source-archive',type=Path,required=True);p.add_argument('--ndk',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();root=Path(__file__).resolve().parents[1]
expected=json.loads((root/'tools/dependency-artifacts.json').read_text())['third_party_sources']['sha256']
assert hashlib.sha256(a.source_archive.read_bytes()).hexdigest()==expected,'Source archive SHA mismatch'
out=a.out.resolve();inputs={'source_archive_sha256':expected,'ndk_source_properties_sha256':hashlib.sha256((a.ndk/'source.properties').read_bytes()).hexdigest(),'build_schema':1}
if out.exists():
 assert a.resume and json.loads((out/'BUILD_INPUTS.json').read_text())==inputs,'Refuse unknown build area or changed inputs'
else:
 out.mkdir(parents=True);(out/'BUILD_INPUTS.json').write_text(json.dumps(inputs,indent=2)+'\n')
src=out/'sources';log=out/'logs';log.mkdir(exist_ok=True)
if not src.exists():
 src.mkdir()
 with tarfile.open(a.source_archive) as tar:tar.extractall(src,filter='data')
source_root=src/'third-party';ndk=a.ndk.resolve();tool=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
values={'CC':str(tool/'aarch64-linux-android28-clang'),'CXX':str(tool/'aarch64-linux-android28-clang++'),'AR':str(tool/'llvm-ar'),'RANLIB':str(tool/'llvm-ranlib'),'NM':str(tool/'llvm-nm'),'STRIP':str(tool/'llvm-strip'),'GPG':str(out/'libgpg-error-install')}
def expand(s):
 for k,v in values.items():s=s.replace('@'+k+'@',v)
 return s
proofs=[]
def run(label,cmd,cwd,env):
 r=subprocess.run([str(x) for x in cmd],cwd=cwd,env=env,capture_output=True,text=True,timeout=900);(log/(label+'.txt')).write_text(r.stdout+r.stderr)
 assert not r.returncode,(label,r.stderr[-2000:]);proofs.append({'step':label,'exit':0})
for recipe in json.loads((root/'tools/dependencies.json').read_text()):
 name=recipe['name'];source=source_root/name;prefix=out/(name+'-install');build=out/(name+'-build');values['PREFIX']=str(prefix)
 done=out/(name+'.done.json');recipe_hash=hashlib.sha256(json.dumps(recipe,sort_keys=True).encode()).hexdigest()
 if done.exists():
  old=json.loads(done.read_text())
  assert old['recipe_sha256']==recipe_hash,'Recipe changed; use a fresh output directory'
  assert all(hashlib.sha256((out/p).read_bytes()).hexdigest()==h for p,h in old['archives'].items()),'Cached archive changed'
  proofs.append({'step':name+'-verified-cache','exit':0});print('PASS verified cache',name,flush=True);continue
 build.mkdir(exist_ok=True)
 # Git exports need the distribution version marker consumed by AX_PACKAGE_VERSION.
 version_file=source/'.tarball-version'
 if not version_file.exists():version_file.write_text(recipe['version']+'\n')
 env={**os.environ,**values,'PATH':str(tool)+os.pathsep+os.environ['PATH'],'ANDROID_NDK_ROOT':str(ndk),'ANDROID_NDK_HOME':str(ndk),'SOURCE_DATE_EPOCH':'1794268800','LC_ALL':'C'}
 env.update({k:expand(v) for k,v in recipe.get('env',{}).items()})
 flags=' -ffile-prefix-map='+str(source)+'=. -fdebug-prefix-map='+str(out)+'=.'
 env['CFLAGS']=env.get('CFLAGS','-O2 -fPIC')+flags;env['CXXFLAGS']=env.get('CXXFLAGS','-O2 -fPIC')+flags
 env['PKG_CONFIG_LIBDIR']=os.pathsep.join(str(x/'lib/pkgconfig') for x in out.glob('*-install'))
 if name=='openssl':
  # OpenSSL Configure requires an in-source build; this copy is disposable.
  work=build/'src';shutil.copytree(source,work)
  run(name+'-configure',['perl','Configure']+[expand(x) for x in recipe['configure']],work,env);where=work
 else:
  if name in ['libplist','libconfig','libsodium'] or not (source/'configure').exists():run(name+'-autoreconf',['autoreconf','-fi'],source,env)
  run(name+'-configure',[source/'configure']+[expand(x) for x in recipe['configure']],build,env);where=build
 run(name+'-make',['make','-j2'],where,env)
 run(name+'-install',['make','install_sw' if name=='openssl' else 'install'],where,env)
 archives={str(p.relative_to(out)):hashlib.sha256(p.read_bytes()).hexdigest() for p in prefix.rglob('*.a')}
 assert archives,'No installed static archive'
 done.write_text(json.dumps({'recipe_sha256':recipe_hash,'archives':archives},indent=2)+'\n')
 print('PASS',name,recipe['version'],flush=True)
(out/'DEPENDENCY_BUILD_RESULT.json').write_text(json.dumps({'pass':True,'source_archive_sha256':expected,'steps':proofs},indent=2)+'\n')
