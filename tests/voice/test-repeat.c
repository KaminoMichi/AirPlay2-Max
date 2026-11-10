#define VP_UNIT
#include "../../src/runtime/voice.c"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
static int listen_fd,mode;
static atomic_int commits,connections;
static void *server(void *unused){
 (void)unused;int fd=accept4(listen_fd,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);assert(fd>=0);
 atomic_fetch_add(&connections,1);yr_packet p;assert(!yr_packet_io(fd,&p,0,yr_now()+1000000000));
 assert(p.magic==YR_MAGIC&&p.version==YR_VERSION&&p.type==YR_HELLO&&p.command==1);
 if(mode==1){p.type=YR_RESULT;p.status=YR_NO_SESSION;}
 else{p.type=YR_READY;p.cid=mode==2?12:11;p.epoch=mode==3?4:3;p.lease=1;p.nonce[0]=2;p.nonce[1]=3;}
 if(mode==6)p.nonce[0]=99;
 if(mode==4)p.magic=0;
 assert(!yr_packet_io(fd,&p,1,yr_now()+500000000));
 if(p.type==YR_READY){
  yr_packet request;
  if(!yr_packet_io(fd,&request,0,yr_now()+1000000000)){
   assert(request.type==YR_COMMIT&&request.cid==p.cid&&request.epoch==p.epoch);
   atomic_fetch_add(&commits,1);
   if(mode!=5){request.type=YR_RESULT;request.status=YR_ACK;assert(!yr_packet_io(fd,&request,1,yr_now()+500000000));}
  }
 }
 close(fd);return NULL;
}
static void rpc_case(int selected,int probe,int expected,int expected_commits){
 mode=selected;atomic_store(&commits,0);atomic_store(&connections,0);
 listen_fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);assert(listen_fd>=0);
 struct sockaddr_un a=yr_address();assert(!bind(listen_fd,(void*)&a,sizeof(a)));assert(!chmod(YR_SOCKET,0600));assert(!listen(listen_fd,1));
 pthread_t thread;assert(!pthread_create(&thread,NULL,server,NULL));
 vp_session pin={.cid=11,.epoch=3,.nonce={2,3},.receiver_pid=getpid(),.receiver_start=vp_start(getpid())};vp_result result;
 if(selected==7)pin.receiver_start++;
 int rc=vp_remote(yr_now(),probe,&pin,&result);assert(rc==expected);
 assert(!pthread_join(thread,NULL));assert(atomic_load(&connections)==1&&atomic_load(&commits)==expected_commits);
 if(selected==5)assert(result.committed&&result.status==YR_UNCERTAIN);
 if(selected==2||selected==3||selected==6||selected==7)assert(!result.committed&&result.status==YR_STALE);
 close(listen_fd);assert(!unlink(YR_SOCKET));
}
int main(int argc,char **argv){
 if(argc==3&&!strcmp(argv[1],"--parse-only")){
  pid_t pid=(pid_t)strtol(argv[2],NULL,10);char *line=NULL;size_t cap=0;unsigned count=0;int64_t wall;
  while(getline(&line,&cap,stdin)>0)if(vp_parse(line,pid,&wall))count++;
  free(line);printf("VP2_PARSE matched_routing_frames=%u no_ipc=true\n",count);return 0;
 }
 if(argc==2&&!strcmp(argv[1],"--fixture-repeat")){
  listen_fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);assert(listen_fd>=0);
  struct sockaddr_un a=yr_address();assert(!bind(listen_fd,(void*)&a,sizeof(a)));assert(!chmod(YR_SOCKET,0600));assert(!listen(listen_fd,2));
  puts("fixture connections=0 commits=0");fflush(stdout);
  for(int k=0;;k++){
   /* initial probe, event1 probe/commit; event2 no-session; later session probe/commit. */
   mode=k==2?1:k>=3?2:0;
   server(NULL);printf("fixture connections=%d commits=%d\n",atomic_load(&connections),atomic_load(&commits));fflush(stdout);
  }
 }
 if(argc==2&&!strcmp(argv[1],"--fixture-serve-two")){
  listen_fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);assert(listen_fd>=0);
  struct sockaddr_un a=yr_address();assert(!bind(listen_fd,(void*)&a,sizeof(a)));assert(!chmod(YR_SOCKET,0600));assert(!listen(listen_fd,2));
  for(int k=0;k<2;k++){server(NULL);printf("fixture connections=%d commits=%d\n",atomic_load(&connections),atomic_load(&commits));fflush(stdout);}
  close(listen_fd);assert(!unlink(YR_SOCKET));return 0;
 }
 int64_t seen=0,tried=0;
 assert(vp_new_event(10000000000LL,&seen,&tried));
 assert(!vp_new_event(10000000000LL,&seen,&tried));
 assert(!vp_new_event(9999999999LL,&seen,&tried));
 assert(!vp_new_event(10500000000LL,&seen,&tried));
 assert(vp_new_event(11000000000LL,&seen,&tried));
 const char good[]="1794313650.123456 25402 25403 I quark-native: [directive_sequencer.cpp:751] routeDirective (fixture000)player_pause payload ignored";
 int64_t wall=0,mapped=0;assert(vp_parse(good,25402,&wall));assert(wall==1794313650123456000LL);
 assert(!vp_parse(good,999,&wall));
 char changed[8192];strcpy(changed,good);strstr(changed,":751")[1]='6';assert(!vp_parse(changed,25402,&wall));
 snprintf(changed,sizeof(changed),"%s_extra",good); /* The suffix is payload, not a second command. */
 assert(vp_parse(changed,25402,&wall));
 strcpy(changed,good);strstr(changed,"player_pause")[6]='X';assert(!vp_parse(changed,25402,&wall));
 strcpy(changed,good);char *suffix=strstr(changed,"player_pause")+12;strcpy(suffix,"_other");assert(!vp_parse(changed,25402,&wall));
 assert(!vp_parse("1794313650.1 25402 3 I Other: [directive_sequencer.cpp:751] routeDirective (x)player_pause",25402,&wall));
 assert(!vp_parse("1794313650.1 25402 3 I quark-native: [other.cpp:1] quoted directive_sequencer.cpp:751] routeDirective (x)player_pause",25402,&wall));
 assert(!vp_parse("1794313650.1 25402 3 I quark-native: {directive_sequencer.cpp:751] routeDirective (x)player_pause",25402,&wall));
 assert(!vp_parse("999999999999999999999.1 25402 3 I quark-native: x",25402,&wall));
 memset(changed,'x',5000);changed[5000]=0;assert(!vp_parse(changed,25402,&wall));
 assert(vp_fresh(10000000000LL,10100000000LL,1100000000LL,9000000000LL,900000000LL,&mapped)&&mapped==1000000000LL);
 assert(!vp_fresh(10000000000LL,10500000000LL,1500000000LL,9000000000LL,900000000LL,&mapped));
 assert(!vp_fresh(10000000000LL,10100000000LL,1100000000LL,9000000000LL,1050000000LL,&mapped));
 assert(!vp_fresh(10200000000LL,10100000000LL,1100000000LL,9000000000LL,900000000LL,&mapped));
 assert(!vp_fresh(10000000000LL,10100000000LL,1100000000LL,8000000000LL,900000000LL,&mapped));
 assert(vp_start(getpid())>0&&vp_wall()>0);
 rpc_case(0,1,0,0); /* Arm probe never COMMITs. */
 rpc_case(0,0,0,1);
 rpc_case(1,0,69,0);
 rpc_case(2,0,75,0);
 rpc_case(3,0,75,0);
 rpc_case(4,0,70,0);
 rpc_case(5,0,70,1); /* Lost reply never causes a retry. */
 rpc_case(6,0,75,0); /* Same CID/epoch from another receiver generation. */
 rpc_case(7,0,75,0); /* A reused receiver PID is not the original process. */
 puts("PASS VP2 parser/freshness +9 real protected IPC cases; process/nonce/generation changes refused, no replay/retry");
 return 0;
}
