#define _GNU_SOURCE
#include "remote-test.h"
#include "remote-io.h"
#include "common.h"
#include "rtsp.h"
#include <fcntl.h>
#include <openssl/rand.h>
#include <stdatomic.h>
#include <stdio.h>
extern ssize_t ap2_event_send_simple_modern_media_remote_command(rtsp_conn_info*,unsigned);
/* Lifetime/generation protected by principal_conn_lock, never by socket input. */
static uint64_t epoch=1,changed_at;
static int authenticated, audio_ptp;
extern const char *ya_remote_payload_variant(void);
static uint64_t nonce[2],serial;
static atomic_int stopping;
static pthread_t worker;
static pthread_mutex_t lifecycle=PTHREAD_MUTEX_INITIALIZER;
static int closing,started,listener=-1;
static dev_t socket_dev;static ino_t socket_ino;
void ya_remote_session_changed(void){
 if(epoch)epoch++;changed_at=(uint64_t)yr_now();
 /* Called by conversation owner under WRITE lock, after pairing and before
  * publishing SETUP/GID. Readers consume only this synchronized snapshot. */
 rtsp_conn_info*c=principal_conn;
 authenticated=c&&c->ap2_pairing_context.event_cipher_bundle.cipher_ctx!=NULL;
 /* Category belongs to the conversation owner too; publish it under WRITE.
  * Never select SETUP (RC) or a different connection from the IPC request. */
 audio_ptp=c&&c->airplay_stream_category==ptp_stream;
}
/* Test synchronized role first: ANNOUNCE publishes classic before the owner
 * changes airplay_type outside this lock. Ineligible roles must not read it. */
static int eligible(rtsp_conn_info*c){return c&&audio_ptp&&authenticated&&epoch&&c->connection_number>0&&c->airplay_type==ap_2&&c->airplay_gid&&strnlen(c->airplay_gid,129)>0&&strnlen(c->airplay_gid,129)<=128;}
static unsigned snapshot(yr_packet*p){
 if(pthread_rwlock_tryrdlock(&principal_conn_lock))return YR_BUSY;
 rtsp_conn_info*c=principal_conn;unsigned r=YR_NO_SESSION;
 if(eligible(c)){r=YR_ACK;if(p->started<changed_at)r=YR_STALE;else{p->cid=(uint32_t)c->connection_number;p->epoch=epoch;}}
 pthread_rwlock_unlock(&principal_conn_lock);return r;
}
static unsigned dispatch(const yr_packet*p){
 if(atomic_load(&stopping))return YR_STOPPING;
 if(pthread_rwlock_tryrdlock(&principal_conn_lock))return YR_BUSY;
 unsigned status=YR_NO_SESSION;rtsp_conn_info*c=principal_conn;
 if(eligible(c)){
  if(p->epoch!=epoch||p->cid!=(uint32_t)c->connection_number||p->started<changed_at)status=YR_STALE;
  else{
   /* Own worker is never asynchronously cancelled: all I/O has deadlines and
    * stop joins it BEFORE RTSP owners release resources. Avoid upstream plist
    * allocation leaks from cancellation inside its unmodified constructor. */
   int64_t old=ya_io_set_deadline(yr_now()+500000000);
   fprintf(stderr,"YA_REMOTE_SEND variant=%s channel=audio_ptp cid=%u epoch=%llu command=%s\n",ya_remote_payload_variant(),p->cid,(unsigned long long)p->epoch,yr_command(p->command));
   errno=0;ssize_t r=ap2_event_send_simple_modern_media_remote_command(c,p->command);int e=errno;
   ya_io_set_deadline(old);
   status=r>0?YR_ACK:e==EBUSY?YR_BUSY:e==EOPNOTSUPP?YR_REJECTED:YR_UNCERTAIN;
  }
 }
 pthread_rwlock_unlock(&principal_conn_lock);return status;
}
static void serve(int fd){
 yr_packet p={0};int64_t begin=yr_now();
 if(yr_root_peer(fd,NULL))return;
 if(yr_packet_io(fd,&p,0,begin+200000000))return;
 yr_packet hello=p;
 if(p.magic!=YR_MAGIC||p.version!=YR_VERSION||p.type!=YR_HELLO||!yr_command_valid(p.command)||p.status||p.cid||p.epoch||p.lease||p.nonce[0]||p.nonce[1]||p.started==0||p.started>(uint64_t)yr_now()||(uint64_t)yr_now()-p.started>800000000){p.status=YR_INVALID;goto reply;}
 p.status=snapshot(&p);if(p.status)goto reply;
 if(++serial==0){atomic_store(&stopping,1);p.status=YR_STOPPING;goto reply;}
 p.lease=serial;p.nonce[0]=nonce[0];p.nonce[1]=nonce[1];p.type=YR_READY;
 if(yr_packet_io(fd,&p,1,begin+250000000))return;
 yr_packet commit={0},wanted=p;wanted.type=YR_COMMIT;
 if(yr_packet_io(fd,&commit,0,begin+400000000))return;
 if(memcmp(&commit,&wanted,sizeof(commit))||(uint64_t)yr_now()-hello.started>800000000)p.status=YR_INVALID;
 else p.status=dispatch(&p);
reply:
 p.type=YR_RESULT;
 fprintf(stderr,"YA_REMOTE command=%s status=%s cid=%u epoch=%llu variant=%s action=UNKNOWN\n",yr_command(p.command),yr_status(p.status),p.cid,(unsigned long long)p.epoch,ya_remote_payload_variant());
 yr_packet_io(fd,&p,1,yr_now()+50000000);
}
static void *run(void*unused){
 (void)unused;pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL);
 while(!atomic_load(&stopping)){
  struct pollfd p={.fd=listener,.events=POLLIN};int r=poll(&p,1,20);if(r<0){if(errno==EINTR)continue;break;}if(!r)continue;
  int fd=accept4(listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);if(fd<0){if(errno==EAGAIN||errno==EINTR)continue;break;}
  serve(fd);close(fd);
 }
 return NULL;
}
static void remove_own_socket(void){struct stat st;if(socket_ino&&!lstat(YR_SOCKET,&st)&&S_ISSOCK(st.st_mode)&&st.st_dev==socket_dev&&st.st_ino==socket_ino)unlink(YR_SOCKET);socket_ino=0;}
static int init_locked(void){
 if(closing)return -ECANCELED;
 if(started||listener>=0)return -EALREADY;
 if(getuid()!=0||yr_private_directory())return -EPERM;
 struct stat st;if(!lstat(YR_SOCKET,&st)||errno!=ENOENT)return -EEXIST;
 if(RAND_bytes((unsigned char*)nonce,sizeof(nonce))!=1)return -EIO;
 listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(listener<0)return -errno;
 struct sockaddr_un a=yr_address();
 if(bind(listener,(void*)&a,sizeof(a))){int e=errno;close(listener);listener=-1;return -e;}
 if(lstat(YR_SOCKET,&st)){close(listener);listener=-1;return -EIO;}socket_dev=st.st_dev;socket_ino=st.st_ino;
 if(chmod(YR_SOCKET,0600)||listen(listener,1)){int e=errno;close(listener);listener=-1;remove_own_socket();return -e;}
 atomic_store(&stopping,0);int r=pthread_create(&worker,NULL,run,NULL);if(r){close(listener);listener=-1;remove_own_socket();return -r;}started=1;return 0;
}
int ya_remote_init(void){pthread_mutex_lock(&lifecycle);int r=init_locked();pthread_mutex_unlock(&lifecycle);return r;}
void ya_remote_stop(void){
 pthread_mutex_lock(&lifecycle);closing=1;
 if(started){atomic_store(&stopping,1);pthread_join(worker,NULL);started=0;}
 if(listener>=0){close(listener);listener=-1;}remove_own_socket();pthread_mutex_unlock(&lifecycle);
}
