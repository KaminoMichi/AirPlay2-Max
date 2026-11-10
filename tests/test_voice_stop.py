#!/usr/bin/env python3
from pathlib import Path
import tempfile,subprocess,os,time,json
R=Path(__file__).resolve().parents[1];results=[]
for case in ['gone','reused_start','wrong_executable','cooperative_stop','live_no_signal']:
 with tempfile.TemporaryDirectory(dir=R.parent/'private',prefix='vstop-') as td:
  d=Path(td);run=d/'run';run.mkdir(mode=0o700);(d/'bin').mkdir();binary=d/'bin/ya-voice';src=d/'fake.c'
  src.write_text('#include <unistd.h>\nint main(void){for(int i=0;i<200;i++){if(!access("'+str(run/'voice.stop')+'",F_OK)){unlink("'+str(run/'voice.active')+'");return 0;}usleep(50000);}return 0;}\n')
  subprocess.run(['gcc',str(src),'-o',str(binary)],check=True)
  proc=None
  try:
   if case=='gone':pid=2147483647;start=1
   else:
    proc=subprocess.Popen(['/bin/sleep','20'] if case=='wrong_executable' else [str(binary)])
    pid=proc.pid;start=int(Path('/proc/'+str(pid)+'/stat').read_text().rpartition(') ')[2].split()[19])
    if case=='reused_start':start+=1
   active=run/'voice.active';active.write_text(f'{pid} {start}\n');active.chmod(0o600)
   script=d/'stop.sh';script.write_text((R/'device/voice-stop.sh').read_text().replace('/data/local/tmp/ya-airplay',str(d)))
   args=['sh',str(script)]+(['--stale-only'] if case=='live_no_signal' else [])
   p=subprocess.run(args,capture_output=True,text=True,timeout=7);want=69 if case=='wrong_executable' else 75 if case=='live_no_signal' else 0
   assert p.returncode==want,(case,p.returncode,p.stdout,p.stderr)
   if case in ['wrong_executable','live_no_signal']:assert active.exists() and proc.poll() is None and not (run/'voice.stop').exists()
   else:assert not active.exists()
   if case=='reused_start':assert proc.poll() is None
   if case=='cooperative_stop':assert proc.wait(timeout=2)==0
   results.append({'case':case,'exit':want,'pass':True})
  finally:
   if proc and proc.poll() is None:proc.terminate();proc.wait(timeout=2)
(R/'build/VOICE_STOP_TESTS.json').write_text(json.dumps({'pass':True,'platform':'Ubuntu owned processes and files; no Android','cases':results},indent=2)+'\n');print('PASS voice ownership cleanup',len(results))
