#define _GNU_SOURCE
#include "remote-io.h"
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#ifndef VP_DIR
#define VP_DIR "/data/local/tmp/ya-airplay"
#endif
#ifndef VP_LOGCAT
#define VP_LOGCAT "/system/bin/logcat"
#endif
typedef struct {uint32_t cid;uint64_t epoch,nonce[2],receiver_start;pid_t receiver_pid;} vp_session;
typedef struct {unsigned status;int committed;} vp_result;
#ifndef VP_UNIT
static volatile sig_atomic_t stop_requested;
static int vp_cancelled(void){
 if(stop_requested)return 1;
 struct stat st;
 if(!lstat(VP_DIR "/run/voice.stop",&st)||errno!=ENOENT){stop_requested=1;return 1;}
 return 0;
}
#else
static int vp_cancelled(void){return 0;}
#endif
/* Fresh routes are ordered, and one utterance's clustered routes are coalesced.
 * A rejected event is never queued or automatically retried. */
static int vp_new_event(int64_t event,int64_t *seen,int64_t *attempted){
 if(event<=*seen)return 0;
 *seen=event;
 if(*attempted&&event-*attempted<1000000000LL)return 0;
 *attempted=event;return 1;
}
static int64_t vp_wall(void){struct timespec t;if(clock_gettime(CLOCK_REALTIME,&t))return 0;return (int64_t)t.tv_sec*1000000000+t.tv_nsec;}
static uint64_t vp_start(pid_t pid){
 char path[64],body[1024];snprintf(path,sizeof(path),"/proc/%d/stat",pid);
 int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return 0;
 ssize_t n=read(fd,body,sizeof(body)-1);close(fd);if(n<=0)return 0;body[n]=0;
 char *end=strrchr(body,')');if(!end||end[1]!=' ')return 0;
 char *save=NULL,*token=strtok_r(end+2," ",&save);
 if(!token||token[0]=='Z'||token[0]=='X')return 0;
 for(int k=0;k<19;k++){token=strtok_r(NULL," ",&save);if(!token)return 0;}
 errno=0;char *last;unsigned long long value=strtoull(token,&last,10);
 return errno||*last?0:(uint64_t)value;
}
/* Ignore opaque parenthesized values; never log or treat them as unique IDs.
 * The first C++ source marker must be the trusted frame, not quoted payload. */
static int vp_parse(const char *line,pid_t wanted,int64_t *wall){
 if(strlen(line)>4096)return 0;
 char *end;errno=0;unsigned long long sec=strtoull(line,&end,10);
 if(errno||end==line||*end++!='.'||sec>(unsigned long long)INT64_MAX/1000000000)return 0;
 int digits=0;int64_t frac=0;
 while(isdigit((unsigned char)*end)){if(++digits>9)return 0;frac=frac*10+(*end++-'0');}
 if(!digits||!isspace((unsigned char)*end))return 0;
 while(digits++<9)frac*=10;
 if(sec>(unsigned long long)(INT64_MAX-frac)/1000000000)return 0;
 long pid,tid;char level,tag[32];int used=0;
 if(sscanf(end," %ld %ld %c %31s%n",&pid,&tid,&level,tag,&used)!=4||pid!=wanted||tid<=0||!strchr("VDIWEF",level)||strcmp(tag,"quark-native:"))return 0;
 const char *body=end+used,*source=strstr(body,"directive_sequencer.cpp:751] routeDirective (");
 if(!source||strstr(body,".cpp:")!=source+strlen("directive_sequencer"))return 0;
 const char *quote=strpbrk(body,"\"'{");if(quote&&quote<source)return 0;
 const char *opaque=source+strlen("directive_sequencer.cpp:751] routeDirective (");
 const char *close=strchr(opaque,')');if(!close||close==opaque||close-opaque>128)return 0;
 for(const char *p=opaque;p<close;p++)if((unsigned char)*p<33||(unsigned char)*p>126)return 0;
 const char *name=close+1;if(strncmp(name,"player_pause",12))return 0;
 unsigned char next=(unsigned char)name[12];if(isalnum(next)||next=='_')return 0;
 *wall=(int64_t)sec*1000000000+frac;return 1;
}
static int vp_fresh(int64_t event,int64_t real,int64_t mono,int64_t offset,int64_t armed,int64_t *mapped){
 int64_t delta=(real-mono)-offset;
 if(delta>50000000||delta< -50000000||event>real||event<real-400000000)return 0;
 *mapped=event-(real-mono);return *mapped>=armed&&*mapped<=mono;
}
/* Same HELLO/READY/COMMIT ABI as the existing CLI. Probe closes READY without
 * COMMIT. Event time (not a new client-launch time) protects session rollover. */
static int vp_remote(int64_t event,int probe,vp_session *pin,vp_result *result){
 int fd=-1,code=70;result->status=YR_UNCERTAIN;result->committed=0;
 if(getuid()!=0||yr_private_directory())return 77;
 struct stat st;if(lstat(YR_SOCKET,&st)||!S_ISSOCK(st.st_mode)||st.st_uid!=0||(st.st_mode&077))return 69;
 fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)goto done;
 struct sockaddr_un a=yr_address();if(connect(fd,(void*)&a,sizeof(a)))goto done;
 pid_t peer;if(yr_root_peer(fd,&peer)){code=77;goto done;}
 char path[64],exe[PATH_MAX];snprintf(path,sizeof(path),"/proc/%d/exe",peer);
 ssize_t n=readlink(path,exe,sizeof(exe)-1);if(n<0)goto done;exe[n]=0;
 if(strcmp(exe,YR_RECEIVER)){code=77;goto done;}
 int64_t began=yr_now();yr_packet p={.magic=YR_MAGIC,.version=YR_VERSION,.type=YR_HELLO,.command=1,.started=(uint64_t)event};
 if(yr_packet_io(fd,&p,1,began+150000000)||yr_packet_io(fd,&p,0,began+300000000))goto done;
 if(p.magic!=YR_MAGIC||p.version!=YR_VERSION||p.command!=1||p.started!=(uint64_t)event)goto done;
 if(p.type==YR_RESULT){result->status=p.status;code=p.status==YR_NO_SESSION?69:75;goto done;}
 if(p.type!=YR_READY||p.status||!p.cid||!p.epoch||!p.lease)goto done;
 uint64_t peer_start=vp_start(peer);if(!peer_start)goto done;
 if(probe){pin->cid=p.cid;pin->epoch=p.epoch;pin->nonce[0]=p.nonce[0];pin->nonce[1]=p.nonce[1];pin->receiver_pid=peer;pin->receiver_start=peer_start;result->status=YR_ACK;code=0;goto done;}
 if(pin->cid!=p.cid||pin->epoch!=p.epoch||pin->nonce[0]!=p.nonce[0]||pin->nonce[1]!=p.nonce[1]||pin->receiver_pid!=peer||pin->receiver_start!=peer_start){result->status=YR_STALE;code=75;goto done;}
 if(yr_now()-event>600000000){result->status=YR_STALE;code=75;goto done;}
 if(vp_cancelled()){result->status=YR_STOPPING;code=75;goto done;}
 yr_packet expected=p;expected.type=YR_RESULT;p.type=YR_COMMIT;
 result->committed=1; /* Never retry from the first COMMIT send attempt. */
 if(yr_packet_io(fd,&p,1,event+700000000)||yr_packet_io(fd,&p,0,began+1300000000))goto done;
 expected.status=p.status;if(memcmp(&p,&expected,sizeof(p)))goto done;
 result->status=p.status;code=p.status==YR_ACK?0:p.status==YR_NO_SESSION?69:75;
done:
 if(fd>=0)close(fd);
 return code;
}
#ifndef VP_UNIT
static void stopped(int sig){(void)sig;stop_requested=1;}
static int vp_source_ok(pid_t pid,uint64_t start){
 char p[64],name[32]={0};snprintf(p,sizeof(p),"/proc/%d/cmdline",pid);
 int fd=open(p,O_RDONLY|O_CLOEXEC);if(fd<0)return 0;
 const char expected[]="com.yandex.io.sdk";ssize_t n=read(fd,name,sizeof(expected));close(fd);
 return n==(ssize_t)sizeof(expected)&&!memcmp(name,expected,sizeof(expected))&&vp_start(pid)==start;
}
static int vp_private_run(void){
 char path[PATH_MAX];struct stat st;
 return realpath(VP_DIR "/run",path)&&!strcmp(path,VP_DIR "/run")&&!stat(path,&st)&&S_ISDIR(st.st_mode)&&st.st_uid==0&&!(st.st_mode&077);
}
static int vp_reap(pid_t child){
 if(child<=0)return 0;
 pid_t initial=waitpid(child,NULL,WNOHANG);
 if(initial==child||(initial<0&&errno==ECHILD))return 0;
 if(initial<0&&errno!=EINTR)return -1;
 kill(child,SIGTERM);
 for(int phase=0;phase<2;phase++){
  int64_t until=yr_now()+300000000;
  do{pid_t n=waitpid(child,NULL,WNOHANG);if(n==child||(n<0&&errno==ECHILD))return 0;if(n<0&&errno!=EINTR)return -1;usleep(10000);}while(yr_now()<until);
  if(!phase)kill(child,SIGKILL);
 }
 return -1;
}
static int vp_observe(pid_t source,uint64_t start,int ready_fd){
 int active=-1,input=-1,code=70;pid_t child=-1;struct stat own={0};
 unsigned long long lines=0,mentions=0,routed=0,stale=0,attempted=0;const char *why="setup_failed";
 if(getuid()!=0||!vp_private_run()||!vp_source_ok(source,start))goto finish;
 active=open(VP_DIR "/run/voice.active",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(active<0)goto finish;
 if(fstat(active,&own)||dprintf(active,"%d %llu\n",getpid(),(unsigned long long)vp_start(getpid()))<0||fsync(active))goto finish;
 vp_session pin={0};vp_result result;
 /* Idle startup is allowed. Each actual event still probes and pins the
  * current receiver/session before its sole COMMIT attempt. */
 int pipes[2];if(pipe2(pipes,O_CLOEXEC))goto finish;
 pid_t owner=getpid();child=fork();if(child<0){close(pipes[0]);close(pipes[1]);goto finish;}
 if(!child){
  if(prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=owner)_exit(126);
  close(pipes[0]);if(dup2(pipes[1],STDOUT_FILENO)<0)_exit(126);close(pipes[1]);
  int null=open("/dev/null",O_RDWR);if(null<0||dup2(null,STDIN_FILENO)<0||dup2(null,STDERR_FILENO)<0)_exit(126);if(null>2)close(null);
  char selected[40];snprintf(selected,sizeof(selected),"--pid=%d",source);
  execl(VP_LOGCAT,"logcat","-T","1",selected,"-v","epoch","quark-native:V","*:S",(char*)NULL);_exit(127);
 }
 close(pipes[1]);input=pipes[0];int flags=fcntl(input,F_GETFL);if(flags<0||fcntl(input,F_SETFL,flags|O_NONBLOCK)<0)goto finish;
 char line[4097];size_t used=0;int oversized=0,armed=0;why="reader_failed";
 int64_t warming=yr_now()+2000000000,arm=0,offset=0,next_stop_check=0,next_source_check=0,last_seen=0,last_attempt=0;
 while(!stop_requested){
  code=70;why="reader_failed";
  if(yr_now()>=next_stop_check){
   struct stat stop_file;
   if(!lstat(VP_DIR "/run/voice.stop",&stop_file)||errno!=ENOENT){stop_requested=1;break;}
   next_stop_check=yr_now()+100000000;
  }
  if(yr_now()>=next_source_check){
   if(!vp_source_ok(source,start)){code=70;why="source_changed";goto finish;}
   next_source_check=yr_now()+1000000000;
  }
  if(!armed&&yr_now()>=warming){
   if(!vp_source_ok(source,start)){code=70;why="source_changed";goto finish;}
   arm=yr_now();offset=vp_wall()-arm;armed=1;
   puts("VP2 status=READY duration=until_stopped command=pause repeat=true cooldown_ms=1000");fflush(stdout);
   if(ready_fd>=0){if(write(ready_fd,"READY",5)!=5)goto finish;close(ready_fd);ready_fd=-1;}
  }
  struct pollfd p={.fd=input,.events=POLLIN};int r=poll(&p,1,25);
  if(r<0){if(errno==EINTR)continue;goto finish;}if(!r)continue;if(p.revents&(POLLNVAL|POLLERR))goto finish;
  char chunk[2048];ssize_t n=read(input,chunk,sizeof(chunk));
  if(n<0){if(errno==EINTR||errno==EAGAIN)continue;goto finish;}if(!n)goto finish;
  for(ssize_t i=0;i<n;i++){
   char c=chunk[i];if(c=='\n'){
    line[used]=0;lines++;
    if(!oversized&&strstr(line,"directive_sequencer.cpp:751]")&&strstr(line,"player_pause"))mentions++;
    int64_t event,mono=yr_now(),mapped=0;
    if(armed&&!stop_requested&&!oversized&&vp_parse(line,source,&event)){
     routed++;
     if(!vp_fresh(event,vp_wall(),mono,offset,arm,&mapped)){stale++;used=0;oversized=0;continue;}
     if(vp_cancelled()){code=0;why="stopped";goto finish;}
     if(!vp_source_ok(source,start)){code=70;why="source_changed";goto finish;}
     if(!vp_new_event(mapped,&last_seen,&last_attempt)){used=0;oversized=0;continue;}
     attempted++;
     /* Re-pin for this event, allowing later sessions but refusing rollover
      * between this probe and COMMIT. Both handshakes carry the event time. */
     code=vp_remote(mapped,1,&pin,&result);
     if(!code)code=vp_remote(mapped,0,&pin,&result);
     if(attempted<=1024)printf("VP2 command=pause status=%s committed=%d exit=%d action=UNKNOWN retry=never\n",yr_status(result.status),result.committed,code);
     else if(attempted==1025)puts("VP2 status=command_logging_capped observer_continues=true");
     fflush(stdout);
     if(code==70||code==77){why="transport_or_auth_failed";goto finish;}
     /* Later, distinct events are allowed. Never retry this event, even if
      * the receiver reports no session, stale, busy or a lost reply. */
    }
    used=0;oversized=0;
   }else if(c==0||used>=sizeof(line)-1)oversized=1;
   else if(!oversized)line[used++]=c;
  }
 }
 code=0;why="stopped";
finish:
 if(ready_fd>=0)close(ready_fd);
 if(input>=0)close(input);
 if(vp_reap(child)){code=70;why="cleanup_failed";}
 if(active>=0){struct stat now;if(!lstat(VP_DIR "/run/voice.active",&now)&&now.st_dev==own.st_dev&&now.st_ino==own.st_ino)unlink(VP_DIR "/run/voice.active");close(active);}
 printf("VP2_SUMMARY lines=%llu route_mentions=%llu routed_frames=%llu stale_frames=%llu attempted=%llu reason=%s\n",lines,mentions,routed,stale,attempted,why);
 printf("VP2 status=finished exit=%d\n",code);fflush(stdout);return code;
}
int main(int argc,char **argv){
 if(argc!=4||(strcmp(argv[1],"--repeat")&&strcmp(argv[1],"--detach-repeat"))){fputs("Usage: ya-voice-pause-repeat --repeat|--detach-repeat IOSDK_PID START_TICKS\n",stderr);return 64;}
 errno=0;char *end;long pid=strtol(argv[2],&end,10);if(errno||*end||pid<=0||pid>INT_MAX)return 64;
 errno=0;uint64_t start=strtoull(argv[3],&end,10);if(errno||*end||!start)return 64;
 struct sigaction sa={.sa_handler=stopped};sigemptyset(&sa.sa_mask);sigaction(SIGTERM,&sa,NULL);sigaction(SIGINT,&sa,NULL);
 /* Do not inherit auto-reaping from a launcher: direct children must remain
  * owned until waitpid, so their PID cannot be reused before cleanup signals. */
 struct sigaction child_sa={.sa_handler=SIG_DFL};sigemptyset(&child_sa.sa_mask);
 if(sigaction(SIGCHLD,&child_sa,NULL))return 70;
 if(!strcmp(argv[1],"--repeat"))return vp_observe((pid_t)pid,start,-1);
 int ready[2];if(pipe2(ready,O_CLOEXEC))return 70;pid_t child=fork();
 if(child<0){close(ready[0]);close(ready[1]);return 70;}
 if(!child){close(ready[0]);if(setsid()<0)_exit(70);int rc=vp_observe((pid_t)pid,start,ready[1]);_exit(rc);}
 close(ready[1]);char result[5];struct pollfd p={.fd=ready[0],.events=POLLIN};
 int r=poll(&p,1,4500);ssize_t n=r>0?read(ready[0],result,sizeof(result)):-1;close(ready[0]);
 if(n==5&&!memcmp(result,"READY",5)){printf("VP2 status=armed child_pid=%d\n",child);return 0;}
 vp_reap(child);fputs("VP2 status=not_armed\n",stderr);return 70;
}
#endif
