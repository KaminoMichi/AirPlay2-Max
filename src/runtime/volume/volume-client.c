#define _GNU_SOURCE
#include "volume.h"
#include "volume-state.h"
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#ifndef YV_APP
#define YV_APP "/system/bin/app_process64"
#endif
#ifndef YV_JAR
#define YV_JAR "/data/local/tmp/ya-airplay/lib/volume-bridge.jar"
#endif
static char *java_env[16];
static void free_environment(void){for(unsigned i=0;i<16;i++){free(java_env[i]);java_env[i]=NULL;}}
static int capture_environment(void){
 const char *keys[]={"ANDROID_ROOT","ANDROID_DATA","BOOTCLASSPATH","DEX2OATBOOTCLASSPATH","ANDROID_RUNTIME_ROOT","ANDROID_TZDATA_ROOT","ANDROID_ART_ROOT","PATH",
#ifndef __ANDROID__
 "YV_TEST_DIR",
#endif
 NULL};
 unsigned used=0;for(unsigned i=0;keys[i];i++){const char*v=getenv(keys[i]);if(!v)continue;size_t n=strlen(keys[i])+strlen(v)+2;java_env[used]=malloc(n);if(!java_env[used]){free_environment();return -ENOMEM;}snprintf(java_env[used++],n,"%s=%s",keys[i],v);}return 0;
}
_Thread_local int ya_rc14_initial_volume;
_Thread_local int ya_rc14_notice_index=-1;
static pthread_mutex_t state_lock=PTHREAD_MUTEX_INITIALIZER;
static ya_volume_state s;
static atomic_int exiting,gate=-1,latched;
static atomic_llong fresh;
static pthread_t worker,notifier;
static int started;
static int64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void publish(void){if(atomic_load(&latched)&&s.active)s.fault=1;atomic_store(&fresh,s.read_at);atomic_store(&gate,yv_gate(&s,now_ms()));}
static void failure(void){pthread_mutex_lock(&state_lock);s.ready=0;if(s.active)s.fault=1;publish();pthread_mutex_unlock(&state_lock);}
/* Child runs only async-signal-safe syscalls until exec. No inherited SPS FDs.
 * PDEATHSIG+parent check closes crash race; SIGCHLD is reaped only by this owner. */
struct linux_dirent64 {uint64_t ino;int64_t off;unsigned short reclen;unsigned char type;char name[];};
static pid_t spawn_java(const char *name){
 char parent[24],classpath[512];snprintf(parent,sizeof(parent),"%d",getpid());snprintf(classpath,sizeof(classpath),"-Djava.class.path=%s",YV_JAR);
 char *args[]={YV_APP,classpath,"/system/bin","--nice-name=ya-volume-rc1.4","ya.station.airplay.volume.VolumeService",(char*)name,parent,NULL};pid_t owner=getpid();
 pid_t p=fork();if(p!=0)return p;
 if(prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=owner)_exit(126);
 int dir=open("/proc/self/fd",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(dir<0)_exit(126);
 _Alignas(8) char buf[4096];long n;
 while((n=syscall(SYS_getdents64,dir,buf,sizeof(buf)))>0){
  for(long at=0;at<n;){struct linux_dirent64*d=(void*)(buf+at);if(d->reclen<20||at+d->reclen>n)_exit(126);int fd=0;const char*q=d->name;int valid=*q>='0'&&*q<='9';for(;*q>='0'&&*q<='9';q++)fd=fd*10+*q-'0';if(valid&&!*q&&fd>2&&fd!=dir)close(fd);at+=d->reclen;}
 }
 close(dir);if(n<0)_exit(126);
 execve(YV_APP,args,java_env);_exit(127);
}
static int reaped(pid_t child){
 int status;pid_t r=waitpid(child,&status,WNOHANG);
 if(r==child){fprintf(stderr,"YA_VOLUME child_exit code=%d signal=%d\n",WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0);return 1;}
 if(r<0&&errno==ECHILD){fprintf(stderr,"YA_VOLUME child_status_unavailable\n");return 1;}
 if(r<0&&errno!=EINTR)_exit(70);
 return 0;
}
static void reap(pid_t child){
 if(child<=0)return;
 int64_t end=now_ms()+1200;
 do{if(reaped(child))return;usleep(10000);}while(now_ms()<end);
 /* An unreaped direct child cannot have its PID reused. */
 kill(child,SIGTERM);end=now_ms()+300;
 do{if(reaped(child))return;usleep(10000);}while(now_ms()<end);
 kill(child,SIGKILL);end=now_ms()+1000;
 do{if(reaped(child))return;usleep(10000);}while(now_ms()<end);
 _exit(70); // kernel failed to reap: refuse unbounded shutdown/unsafe ownership
}
static int connect_java(const char *name,pid_t child){
 int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return -1;
 struct sockaddr_un a={.sun_family=AF_UNIX};size_t n=strlen(name);if(n+1>sizeof(a.sun_path)){close(fd);return -1;}memcpy(a.sun_path+1,name,n);
 if(connect(fd,(void*)&a,(socklen_t)(offsetof(struct sockaddr_un,sun_path)+1+n))){close(fd);return -1;}
 struct ucred peer;socklen_t len=sizeof(peer);
 if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&len)||len!=sizeof(peer)||peer.uid!=getuid()||peer.pid!=child){close(fd);errno=EPERM;return -1;}return fd;
}
/* Absolute deadline includes partial writes/reads and readiness races. */
static int transact(int fd,unsigned seq,int target,int *index){
 char request[48],reply[64];int len=target<0?snprintf(request,sizeof(request),"G %u\n",seq):snprintf(request,sizeof(request),"S %u %d\n",seq,target);
 int sent=0,used=0;int64_t end=now_ms()+500;
 while(sent<len||used<(int)sizeof(reply)-1){
  if(atomic_load(&exiting)||now_ms()>=end)return -1;
  struct pollfd p={.fd=fd,.events=sent<len?POLLOUT:POLLIN};int rc=poll(&p,1,20);if(rc<0){if(errno==EINTR)continue;return -1;}if(!rc)continue;
  if(p.revents&(POLLNVAL|POLLERR))return -1;
  if(sent<len){ssize_t n=send(fd,request+sent,(size_t)(len-sent),MSG_DONTWAIT|MSG_NOSIGNAL);if(n>0)sent+=(int)n;else if(n==0||(errno!=EAGAIN&&errno!=EINTR))return -1;}
  else{char c;ssize_t n=recv(fd,&c,1,MSG_DONTWAIT);if(n>0){if(c=='\n'){reply[used]=0;unsigned got;int version,min,max,idx,consumed=0;if(sscanf(reply,"V %d %u %d %d %d%n",&version,&got,&idx,&min,&max,&consumed)!=5||reply[consumed]||version!=1||got!=seq||min!=0||max!=100||idx<0||idx>100)return -1;if(target>=0&&idx!=target)return -1;*index=idx;return 0;}reply[used++]=c;if(used==(int)sizeof(reply)-1)return -1;}
   else if(n==0||(errno!=EAGAIN&&errno!=EINTR))return -1;}
 }
 return -1;
}
static void reconnect_pause(unsigned *failures){
 unsigned delay=500u<<(*failures>6?6:*failures);if(delay>30000)delay=30000;if(*failures<6)(*failures)++;
 int64_t end=now_ms()+delay;while(!atomic_load(&exiting)&&now_ms()<end)usleep(20000);
}
static void *client_main(void *unused){
 (void)unused;unsigned seq=0;pid_t child=0;int fd=-1,previous=-1;int64_t next_get=0,healthy_since=0;unsigned failures=0;
 while(!atomic_load(&exiting)){
  if(fd<0){
   char name[96];snprintf(name,sizeof(name),"ya-airplay-volume-v14-%d",getpid());child=spawn_java(name);int64_t end=now_ms()+3000;
   while(child>0&&fd<0&&!atomic_load(&exiting)&&now_ms()<end){fd=connect_java(name,child);if(fd<0)usleep(20000);}
   if(fd<0){failure();reap(child);child=0;reconnect_pause(&failures);continue;}next_get=0;
  }
  uint64_t epoch,request;int target=-1;int64_t now=now_ms();
  pthread_mutex_lock(&state_lock);if(s.active&&s.pending&&!s.fault&&now>=s.due){target=s.target;s.inflight=1;}epoch=s.epoch;request=target<0?0:s.request;pthread_mutex_unlock(&state_lock);
  if(target<0&&now<next_get){usleep(10000);continue;}
  int idx=0;seq=seq>=999999999?1:seq+1;int rc=transact(fd,seq,target,&idx);
  // Publish failure BEFORE releasing the in-flight lease. A concurrent end/
  // begin must never see a completed RPC paired with the previous healthy state.
  pthread_mutex_lock(&state_lock);
  yv_observe(&s,rc==0?idx:-1,epoch,request,now_ms());
  s.inflight=0;publish();pthread_mutex_unlock(&state_lock);
  if(rc&&!atomic_load(&exiting))fprintf(stderr,"YA_VOLUME transport_failed; current session latched closed\n");
  if(!rc&&target>=0)fprintf(stderr,"YA_VOLUME applied index=%d\n",idx);
  if(!rc&&idx!=previous){fprintf(stderr,"YA_VOLUME observed index=%d range=0..100\n",idx);previous=idx;}
  if(rc){close(fd);fd=-1;reap(child);child=0;healthy_since=0;if(!atomic_load(&exiting))reconnect_pause(&failures);}
  else{next_get=now_ms()+200;if(!healthy_since)healthy_since=now_ms();if(now_ms()-healthy_since>=5000)failures=0;}
 }
 if(fd>=0)close(fd);
 reap(child);return NULL;
}
static void *notify_main(void *unused){
 (void)unused;uint64_t epoch=0,notice=0,disabled_epoch=0;int last=-1;
 while(!atomic_load(&exiting)){
  usleep(30000);int cid,idx,active;uint64_t ep,n;
  pthread_mutex_lock(&state_lock);active=s.active&&s.ready&&!s.fault;cid=s.cid;idx=s.current;ep=s.epoch;n=s.notice;pthread_mutex_unlock(&state_lock);
  if(!active){last=-1;continue;}if(disabled_epoch==ep)continue;if(epoch!=ep){epoch=ep;notice=0;last=-1;}
  if(notice==n){last=idx;continue;}
  // Unknown sender state: initial notification is actual Android value. Later
  // changes use upstream's <=0.09 steps (Tahoe workaround), never AudioService.
  int step=last<0?idx:(idx-last>9?last+9:(last-idx>9?last-9:idx));
  pthread_mutex_lock(&state_lock);
  int still_current=s.active&&s.epoch==ep&&s.cid==cid;
  if(still_current)s.notifying=1;
  pthread_mutex_unlock(&state_lock);
  if(!still_current)continue;
  int rc=ya_volume_notify(cid,step/100.0);
  pthread_mutex_lock(&state_lock);s.notifying=0;pthread_mutex_unlock(&state_lock);
  if(rc==0)usleep(170000); // no event channel/busy: bounded low-load retry
  if(rc==-2){disabled_epoch=ep;fprintf(stderr,"YA_VOLUME reverse_unsupported; forward control retained\n");}
  if(rc>0){last=step;fprintf(stderr,"YA_VOLUME notified index=%d\n",step);if(last==idx)notice=n;}
 }
 return NULL;
}
int ya_volume_init(void){
 if(started)return -EALREADY;
 int environment_rc=capture_environment();if(environment_rc)return environment_rc;
 memset(&s,0,sizeof(s));atomic_store(&exiting,0);atomic_store(&latched,0);atomic_store(&gate,-1);
 int r=pthread_create(&worker,NULL,client_main,NULL);if(r){free_environment();return -r;}started=1;
 r=pthread_create(&notifier,NULL,notify_main,NULL);if(r){atomic_store(&exiting,1);pthread_join(worker,NULL);started=0;free_environment();return -r;}started=2;
 int64_t end=now_ms()+4000;while(now_ms()<end){pthread_mutex_lock(&state_lock);int ready=s.ready;pthread_mutex_unlock(&state_lock);if(ready)return 0;usleep(20000);}
 ya_volume_deinit();return -ENODEV;
}
void ya_volume_deinit(void){if(!started)return;atomic_store(&exiting,1);pthread_join(worker,NULL);if(started==2)pthread_join(notifier,NULL);started=0;free_environment();}
void ya_volume_begin(int cid){pthread_mutex_lock(&state_lock);atomic_store(&latched,0);yv_begin(&s,cid,now_ms());publish();pthread_mutex_unlock(&state_lock);}
void ya_volume_end(void){
 pthread_mutex_lock(&state_lock);yv_end(&s);publish();pthread_mutex_unlock(&state_lock);
 int64_t end=now_ms()+1000;for(;;){pthread_mutex_lock(&state_lock);int busy=s.inflight||s.notifying;pthread_mutex_unlock(&state_lock);if(!busy||now_ms()>=end)break;usleep(10000);}
}
void ya_volume_request(int index){pthread_mutex_lock(&state_lock);yv_request(&s,index,ya_rc14_initial_volume,now_ms());publish();pthread_mutex_unlock(&state_lock);}
double ya_volume_initial_db(void){pthread_mutex_lock(&state_lock);int idx=s.current;pthread_mutex_unlock(&state_lock);return idx?idx*0.3-30.0:-144.0;}
int ya_volume_gate(void){int g=atomic_load(&gate);if(g<0||now_ms()-atomic_load(&fresh)>1000){atomic_store(&latched,1);return -1;}return g;}

void ya_volume_mark_echo(void){int idx=ya_rc14_notice_index;if(idx<0||idx>100)return;pthread_mutex_lock(&state_lock);if(s.active)s.echo_until[idx]=now_ms()+1500;pthread_mutex_unlock(&state_lock);}
