#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""ADB installer/controller. No device action occurs on import or --help."""
from pathlib import Path,PurePosixPath
import argparse,hashlib,json,os,re,secrets,shlex,subprocess,sys,tarfile,time
ROOT='/data/local/tmp/ya-airplay'
FINGERPRINT='Yandex/yandexstation_2/yandexstation_2:9/PPR1.180610.011/5000000921:user/release-keys'
class Refusal(RuntimeError):pass

def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def validate_bundle(folder):
 folder=Path(folder);meta=json.loads((folder/'release.json').read_text())
 if meta['version']!='0.1.0-rc.1':raise Refusal('Неподдерживаемая версия пакета')
 archive=folder/'payload.tar';script=folder/'manage.sh'
 if digest(folder/'ya-publish')!=meta['publisher_sha256']:raise Refusal('SHA-256 publisher не совпадает')
 if digest(archive)!=meta['payload_sha256'] or digest(script)!=meta['manage_sha256']:raise Refusal('SHA-256 пакета не совпадает')
 with tarfile.open(archive) as tar:
  members=tar.getmembers();names=set();total=0;files={}
  if len(members)>200:raise Refusal('Слишком много файлов в пакете')
  for m in members:
   p=PurePosixPath(m.name)
   if not m.isfile() or p.is_absolute() or '..' in p.parts or m.name in names or str(p)!=m.name:raise Refusal('Небезопасный путь/тип в архиве')
   if not re.fullmatch(r'[A-Za-z0-9_.\-/]+',m.name):raise Refusal('Неподдерживаемое имя файла')
   names.add(m.name);total+=m.size
   if total>64*1024*1024:raise Refusal('Пакет превышает допустимый размер')
   files[m.name]=hashlib.sha256(tar.extractfile(m).read()).hexdigest()
  expected=meta['files']
  if files!=expected:raise Refusal('Состав архива не совпадает с manifest')
 return meta

def compatibility(props,free_kib):
 expected={'ro.product.device':'yandexstation_2','ro.build.version.incremental':'5000000921','ro.build.version.sdk':'28','ro.product.cpu.abi':'arm64-v8a','ro.build.fingerprint':FINGERPRINT}
 for k,v in expected.items():
  if props.get(k)!=v:raise Refusal('Несовместимость: '+k+'; установка остановлена')
 if props.get('selinux')!='Permissive':raise Refusal('SELinux Enforcing не проверен. Установщик его не переключает.')
 if free_kib<256*1024:raise Refusal('Недостаточно места: нужно не менее 256 MiB в /data')

def root_metadata(text):
 result={}
 for line in text.splitlines():
  k,sep,v=line.partition('=')
  if sep and k in {'id','status','result','exit','signal','truncated','dropped','time'}:
   if k in result:raise Refusal('Повтор поля root-result')
   result[k]=v
 return result

class Adb:
 def __init__(self,serial=None,program='adb'):
  self.command=[program]+(['-s',serial] if serial else [])
 def run(self,args,timeout=20,check=True):
  r=subprocess.run(self.command+args,capture_output=True,text=True,timeout=timeout)
  if check and r.returncode:raise Refusal('ADB: '+(r.stderr.strip() or r.stdout.strip() or str(r.returncode)))
  return r
 def shell(self,script,**kw):return self.run(['shell',script],**kw)
 def read_preflight(self):
  props={}
  for k in ['ro.product.device','ro.build.version.incremental','ro.build.version.sdk','ro.product.cpu.abi','ro.build.fingerprint']:
   props[k]=self.shell('getprop '+k).stdout.strip()
  props['selinux']=self.shell('getenforce').stdout.strip()
  rows=self.shell('df -Pk /data').stdout.splitlines()
  try:free=int(rows[-1].split()[-3])
  except (IndexError,ValueError):raise Refusal('Не удалось достоверно проверить свободное место')
  compatibility(props,free)
  return {'compatible_properties':True,'free_mib':free//1024,'model_note':'SKU должен подтвердить владелец; yandexstation_2 сам по себе не доказывает YNDX-00052'}

class Root:
 def __init__(self,adb,method,publisher):self.adb=adb;self.method=method;self.publisher=publisher
 def execute(self,script):
  if self.method=='shell':return self.adb.shell(script,timeout=65).stdout
  if self.method=='su':return self.adb.shell('su 0 sh -c '+shlex.quote(script),timeout=65).stdout
  return self.queue(script)
 def queue(self,script):
  # Existing Hybrid Root only; never installs or changes rootd. A unique audit
  # directory and strict result attribution avoid reuse of shared old output.
  tx=secrets.token_hex(8);base='/data/local/tmp/ya-airplay-root-'+tx;begin='YA_PUBLIC_'+tx+'_BEGIN';end='YA_PUBLIC_'+tx+'_END'
  payload='printf "%s\\n" '+shlex.quote(begin)+'\nset -eu\numask 077\n'+script+'\nprintf "%s\\n" '+shlex.quote(end)+'\n'
  if len(payload.encode())>7000:raise Refusal('Слишком большой root-запрос')
  self.adb.shell('umask 077; mkdir -m 700 '+shlex.quote(base))
  self.adb.run(['push',str(self.publisher),base+'/publish'])
  expected=digest(self.publisher)
  self.adb.shell('v=$(sha256sum '+base+'/publish); [ "${v%% *}" = '+expected+' ] && chmod 700 '+base+'/publish')
  command='''set -eu
umask 077
[ "$(getprop init.svc.rootd)" = running ]
[ ! -e /data/local/tmp/rootcmd ] && [ ! -L /data/local/tmp/rootcmd ]
set -- /data/local/tmp/rootcmd-*
[ "$1" = '/data/local/tmp/rootcmd-*' ]
B=%s
[ -d "$B" ] && [ ! -L "$B" ]
if [ -L /data/local/tmp/rootres.default ]; then exit 65
elif [ -f /data/local/tmp/rootres.default ]; then cat /data/local/tmp/rootres.default > "$B/before"
elif [ ! -e /data/local/tmp/rootres.default ]; then printf 'ABSENT\\n' > "$B/before"
else exit 65; fi
cat > "$B/request.part" <<'YA_PUBLIC_REQUEST'
%sYA_PUBLIC_REQUEST
chmod 600 "$B/request.part"
[ ! -e /data/local/tmp/rootcmd ] && [ ! -L /data/local/tmp/rootcmd ]
"$B/publish" "$B/request.part"
'''%(shlex.quote(base),payload)
  self.adb.shell(command,timeout=15) # Exactly one publication; never retry.
  before=self.adb.shell('cat '+shlex.quote(base+'/before')).stdout
  deadline=time.monotonic()+50
  while time.monotonic()<deadline:
   readmeta='[ -f /data/local/tmp/rootres.default ] && [ ! -L /data/local/tmp/rootres.default ] && cat /data/local/tmp/rootres.default'
   first=self.adb.shell(readmeta,check=False).stdout
   output=self.adb.shell('[ ! -L /data/local/tmp/rootout ] && cat /data/local/tmp/rootout',check=False).stdout
   last=self.adb.shell(readmeta,check=False).stdout
   m=root_metadata(last)
   fresh=first==last and last!=before and m.get('time','')>root_metadata(before).get('time','')
   complete=output.count(begin)==1 and output.count(end)==1 and output.index(begin)<output.index(end)
   if fresh and m.get('status')=='executed' and m.get('result')=='exited':
    if not complete or m.get('id')!='default' or any(m.get(k)!='0' for k in ['exit','signal','truncated','dropped']):
     raise Refusal('Root-операция завершилась с кодом '+m.get('exit','unknown')+'; повтор запрещён. Результаты: '+base+('\n'+output.split(begin,1)[1][-3000:] if output.count(begin)==1 else ''))
    return output.split(begin,1)[1].split(end,1)[0].strip()
   if fresh and m.get('status')=='refused':raise Refusal('Rootd отклонил запрос; повтор запрещён, результаты: '+base)
   time.sleep(.5)
  raise Refusal('Нет свежего полного root-result. Не повторяйте операцию вслепую; результаты: '+base)

def main(argv=None):
 p=argparse.ArgumentParser(description='YA AirPlay 2 — установка и управление через существующий ADB/root')
 p.add_argument('operation',choices=['check','install','update','status','start','stop','rollback','uninstall','play','pause','next','previous','autostart-on','autostart-off','voice-start','voice-stop'])
 p.add_argument('--serial');p.add_argument('--adb',default='adb');p.add_argument('--root',choices=['auto','shell','su','rootd'],default='auto')
 p.add_argument('--bundle',type=Path,default=Path(__file__).resolve().parents[1]/'release');p.add_argument('--autostart',choices=['off','required'],default='off');p.add_argument('--hybrid-root-version',choices=['1.2.4']);p.add_argument('--voice',choices=['off','on'],default='off');p.add_argument('--model',choices=['YNDX-00052']);p.add_argument('--yes',action='store_true')
 a=p.parse_args(argv)
 adb=Adb(a.serial,a.adb);print(json.dumps(adb.read_preflight(),ensure_ascii=False,indent=2))
 if a.operation=='check':return 0
 method=a.root
 if method=='auto':
  if adb.shell('id -u').stdout.strip()=='0':method='shell'
  elif adb.shell('command -v su',check=False).returncode==0:method='su'
  elif adb.shell('getprop init.svc.rootd').stdout.strip()=='running' and a.hybrid_root_version=='1.2.4':method='rootd'
  else:raise Refusal('Не подтверждён root. Для существующего Hybrid Root 1.2.4 укажите --root rootd --hybrid-root-version 1.2.4; получение root не входит в установщик.')
 if method=='rootd' and a.hybrid_root_version!='1.2.4':raise Refusal('Поддерживается только явно подтверждённый Hybrid Root 1.2.4')
 if a.operation in ['install','update']:
  meta=validate_bundle(a.bundle)
  if not a.model:raise Refusal('Укажите --model YNDX-00052 после сверки модели на корпусе')
  print('Установка только в /data/local/tmp/ya-airplay, наш NSD APK; backup и ограниченный откат. Автозапуск: '+a.autostart)
  print('Это release candidate: длительная стабильность и установка на устройстве ещё требуют испытаний.')
 else:print('Операция: '+a.operation+'; только компоненты YA AirPlay.')
 if not a.yes and input('Для продолжения введите YES: ').strip()!='YES':raise Refusal('Отменено владельцем; изменений нет')
 meta=validate_bundle(a.bundle)
 root=Root(adb,method,a.bundle/'ya-publish')
 if a.operation in ['install','update']:
  tx=secrets.token_hex(8);stage='/data/local/tmp/ya-airplay-stage-'+tx
  adb.shell('umask 077; mkdir -m 700 '+stage)
  for name in ['payload.tar','manage.sh']:adb.run(['push',str(a.bundle/name),stage+'/'+name],timeout=60)
  verify='set -eu\n'
  for name,wanted in [('payload.tar',meta['payload_sha256']),('manage.sh',meta['manage_sha256'])]:
   path=stage+'/'+name;verify+='[ -f '+path+' ] && [ ! -L '+path+' ]\nv=$(sha256sum '+path+'); [ "${v%% *}" = '+wanted+' ]\n'
  output=root.execute(verify+'/system/bin/sh '+stage+'/manage.sh '+a.operation+' '+a.autostart+' '+a.voice)
 else:
  # No arbitrary path or free-form root command is accepted by this CLI.
  output=root.execute('/system/bin/sh '+ROOT+'/manage.sh '+a.operation)
 print(output)
 return 0
if __name__=='__main__':
 try:raise SystemExit(main())
 except (Refusal,OSError,ValueError,subprocess.TimeoutExpired,KeyError) as e:print('STOP: '+str(e),file=sys.stderr);raise SystemExit(1)
