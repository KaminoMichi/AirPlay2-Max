"""Real host process/socket fixtures only, not device tests."""
from pathlib import Path
import subprocess,os,time,select

def lifecycle(w,fixture,run):
 fake=w/'out/fake-logcat.py'
 fake.write_text("""#!/usr/bin/python3
import sys,time
pid=next(x.split('=')[1] for x in sys.argv if x.startswith('--pid='))
time.sleep(3)
for group in range(3):
 for _ in range(3):
  t=time.time_ns();print(f"{t//1000000000}.{t%1000000000:09d} {pid} {pid} I quark-native: [directive_sequencer.cpp:751] routeDirective (fixture000)player_pause",flush=True)
 time.sleep(1.3)
time.sleep(30)
""");fake.chmod(0o755)
 vp=fixture/'vp';vp.mkdir(mode=0o700);(vp/'run').mkdir(mode=0o700)
 binary=w/'out/host-observer'
 run('lifecycle-build',['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-DVP_DIR="'+str(vp)+'"','-DYR_ROOT="'+str(fixture)+'"','-DVP_LOGCAT="'+str(fake)+'"','-I'+str(w/'runtime'),w/'src/runtime/voice.c','-o',binary])
 env=os.environ.copy();env.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
 def ticks(pid):return Path('/proc/'+str(pid)+'/stat').read_text().rpartition(') ')[2].split()[19]
 reports=[]
 for mode in ['repeat_reconnect','cancel_before_event','stop_file_before_event','source_restart','source_restart_after_event']:
  source=subprocess.Popen(['com.yandex.io.sdk','30'],executable='/bin/sleep')
  server=subprocess.Popen([str(fixture/'bin/ya-shairport'),'--fixture-repeat'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env)
  helper=None
  try:
   until=time.monotonic()+3
   while not (fixture/'run/remote-test.sock').exists():
    assert server.poll() is None;assert time.monotonic()<until;time.sleep(.01)
   helper=subprocess.Popen([str(binary),'--repeat',str(source.pid),ticks(source.pid)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env)
   prefix=[];until=time.monotonic()+5
   while True:
    assert time.monotonic()<until,'READY missing'
    ready,_,_=select.select([helper.stdout],[],[],.1)
    if ready:
     line=helper.stdout.readline();prefix.append(line)
     if 'status=READY' in line:break
    assert helper.poll() is None,'observer exited before READY'
   if mode=='repeat_reconnect':
    time.sleep(5)
    assert helper.poll() is None,'must survive multiple commands and no-session'
    (vp/'run/voice.stop').write_text('STOP\n')
   elif mode=='stop_file_before_event':(vp/'run/voice.stop').write_text('STOP\n')
   elif mode in ['source_restart','source_restart_after_event']:
    if mode=='source_restart_after_event':time.sleep(1.5)
    source.terminate();source.wait(timeout=2)
   else:helper.terminate()
   out,err=helper.communicate(timeout=4);out=''.join(prefix)+out
   server.terminate();srv,serr=server.communicate(timeout=3)
   if mode=='repeat_reconnect':
    assert helper.returncode==0,(out,err)
    assert out.count('command=pause status=rtsp_ack committed=1')==2,(out,srv)
    assert out.count('command=pause status=no_session committed=0')==1,(out,srv)
    assert 'connections=5 commits=2' in srv,(out,srv)
   elif mode in ['source_restart','source_restart_after_event']:assert helper.returncode==70 and 'reason=source_changed' in out,(out,err)
   else:assert helper.returncode==0 and 'command=pause status=' not in out,(out,err)
   if mode not in ['repeat_reconnect','source_restart_after_event']:assert 'connections=0 commits=0' in srv,(out,srv)
   if mode not in ['source_restart','source_restart_after_event']:assert source.poll() is None,'source must not be signaled'
   assert not (vp/'run/voice.active').exists()
   assert not err and not serr,(err,serr)
   reports.append({'case':mode,'observer_stdout':out,'fixture_stdout':srv,'active_removed':True})
  finally:
   for p in [helper,server,source]:
    if p is not None and p.poll() is None:p.terminate()
   for p in [helper,server,source]:
    if p is not None:
     try:p.wait(timeout=3)
     except subprocess.TimeoutExpired:p.kill();p.wait(timeout=3)
   for p in [fixture/'run/remote-test.sock',vp/'run/voice.stop']:
    if p.exists():p.unlink()
 return reports
