#define _GNU_SOURCE
#define YA_THREAD_IMPLEMENTATION
#include "ya-thread.h"
#include "ya-stop.h"
#include <stdatomic.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/eventfd.h>
/* Registry owns each context from before create until after real join.
 * cancel never closes a target-owned FD. No signals/asynchronous unwinding.
 * Every selected worker is joinable. Unsupported detached creates fail explicitly.
 */
typedef struct worker {
 pthread_t tid; ya_stop stop; int done_fd,joining;
 atomic_int done;
 int disabled,exiting;
 void *(*entry)(void *);void *arg;
 struct worker *next;
} worker;
static pthread_mutex_t registry_lock=PTHREAD_MUTEX_INITIALIZER;
static worker *registry;
static _Thread_local worker *self;
static _Thread_local int main_disabled;
static _Thread_local int64_t io_deadline;
int64_t ya_io_set_deadline(int64_t value){int64_t old=io_deadline;io_deadline=value;return old;}
int64_t ya_io_get_deadline(void){return io_deadline;}
static int64_t mono_ns(void){struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t))abort();return (int64_t)t.tv_sec*1000000000LL+t.tv_nsec;}
static worker *lookup(pthread_t t){for(worker*w=registry;w;w=w->next)if(pthread_equal(w->tid,t))return w;return NULL;}
size_t ya_thread_live_count(void){size_t n=0;pthread_mutex_lock(&registry_lock);for(worker*w=registry;w;w=w->next)n++;pthread_mutex_unlock(&registry_lock);return n;}
static void finish(void *p){worker*w=p;w->exiting=1;atomic_store_explicit(&w->done,1,memory_order_release);uint64_t one=1;ssize_t rc;do{rc=write(w->done_fd,&one,8);}while(rc<0&&errno==EINTR);}
static void *start(void*p){worker*w=p;self=w;
 /* Publication barrier: registry and tid must exist before entry executes. */
 pthread_mutex_lock(&registry_lock);pthread_mutex_unlock(&registry_lock);
 void *result;
 pthread_cleanup_push(finish,w);
 result=w->entry(w->arg);
 pthread_cleanup_pop(1);
 return result;
}
int ya_thread_create(pthread_t*t,const pthread_attr_t*a,void*(*entry)(void*),void*arg){
 if(!t||!entry)return EINVAL;
 int detached=PTHREAD_CREATE_JOINABLE;if(a){int r=pthread_attr_getdetachstate(a,&detached);if(r)return r;}
 if(detached!=PTHREAD_CREATE_JOINABLE)return ENOTSUP;
 worker*w=calloc(1,sizeof(*w));if(!w)return ENOMEM;
 if(ya_stop_init(&w->stop)){int e=errno;free(w);return e;}
 w->done_fd=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);if(w->done_fd<0){int e=errno;ya_stop_destroy(&w->stop);free(w);return e;}
 atomic_init(&w->done,0);w->entry=entry;w->arg=arg;
 pthread_mutex_lock(&registry_lock);
 int rc=pthread_create(&w->tid,a,start,w);
 if(!rc){w->next=registry;registry=w;*t=w->tid;}
 pthread_mutex_unlock(&registry_lock);
 if(rc){close(w->done_fd);ya_stop_destroy(&w->stop);free(w);}return rc;
}
int ya_thread_cancel(pthread_t t){
 pthread_mutex_lock(&registry_lock);worker*w=lookup(t);int rc=ESRCH;
 if(w)rc=ya_stop_request(&w->stop)?errno:0;
 pthread_mutex_unlock(&registry_lock);return rc;
}
void ya_thread_testcancel(void){
 if(self&&!self->disabled&&!self->exiting&&atomic_load_explicit(&self->stop.requested,memory_order_acquire)){
  self->exiting=1;self->disabled=1;
  pthread_exit(PTHREAD_CANCELED); /* Bionic executes existing cleanup stack. */
 }
}
int ya_thread_setcancelstate(int state,int*old){
 if(state!=PTHREAD_CANCEL_ENABLE&&state!=PTHREAD_CANCEL_DISABLE)return EINVAL;
 int*p=self?&self->disabled:&main_disabled;
 if(old)*old=*p?PTHREAD_CANCEL_DISABLE:PTHREAD_CANCEL_ENABLE;
 *p=state==PTHREAD_CANCEL_DISABLE;
 if(state==PTHREAD_CANCEL_ENABLE)ya_thread_testcancel();
 return 0;
}
int ya_thread_join(pthread_t t,void**result){
 if(pthread_equal(t,pthread_self()))return EDEADLK;
 pthread_mutex_lock(&registry_lock);worker*w=lookup(t);
 if(!w){pthread_mutex_unlock(&registry_lock);return ESRCH;}
 if(w->joining){pthread_mutex_unlock(&registry_lock);return EBUSY;}
 w->joining=1;pthread_mutex_unlock(&registry_lock);
 int64_t deadline=mono_ns()+10000000000LL;
 while(!atomic_load_explicit(&w->done,memory_order_acquire)){
  /* Do not abandon the registry claim if the joiner is stopped. Upstream
   * callers disable cancellation while owning children; enforce that here. */
  struct pollfd p={.fd=w->done_fd,.events=POLLIN};poll(&p,1,20);
  if(mono_ns()>deadline){
   static const char text[]="FATAL: worker failed 10-second shutdown deadline; refusing unsafe free\n";
   ssize_t diagnostic_bytes=write(STDERR_FILENO,text,sizeof(text)-1);
   (void)diagnostic_bytes;_exit(70);
  }
 }
 int rc=pthread_join(t,result);
 pthread_mutex_lock(&registry_lock);
 if(!rc){worker**p=&registry;while(*p&&*p!=w)p=&(*p)->next;if(*p)*p=w->next;}
 else w->joining=0;
 pthread_mutex_unlock(&registry_lock);
 if(!rc){close(w->done_fd);ya_stop_destroy(&w->stop);free(w);}return rc;
}
int ya_poll(struct pollfd*f,nfds_t n,int timeout){
 ya_thread_testcancel();
 if(!self||self->disabled||self->exiting)return poll(f,n,timeout);
 int r=ya_stop_wait(&self->stop,f,n,timeout);int e=errno;ya_thread_testcancel();errno=e;return r;
}
int ya_select(int nfds,fd_set*r,fd_set*w,fd_set*e,struct timeval*tv){
 if(nfds<0||nfds>FD_SETSIZE){errno=EINVAL;return -1;}
 if(tv&&(tv->tv_sec<0||tv->tv_usec<0||tv->tv_usec>=1000000)){errno=EINVAL;return -1;}
 struct pollfd f[FD_SETSIZE];int n=0;
 for(int fd=0;fd<nfds;fd++){short ev=0;if(r&&FD_ISSET(fd,r))ev|=POLLIN;if(w&&FD_ISSET(fd,w))ev|=POLLOUT;if(e&&FD_ISSET(fd,e))ev|=POLLPRI;if(ev)f[n++]=(struct pollfd){.fd=fd,.events=ev};}
 int64_t ms=tv?(int64_t)tv->tv_sec*1000+(tv->tv_usec+999)/1000:-1;
 int rc=ya_poll(f,n,ms>INT_MAX?INT_MAX:(int)ms);if(rc<0)return rc;
 if(r)FD_ZERO(r);
 if(w)FD_ZERO(w);
 if(e)FD_ZERO(e);
 int ready=0;
 for(int i=0;i<n;i++){
  short v=f[i].revents;int fd=f[i].fd;
  if(v&POLLNVAL){errno=EBADF;return -1;}
  if(r&&(f[i].events&POLLIN)&&(v&(POLLIN|POLLHUP|POLLERR))){FD_SET(fd,r);ready++;}
  if(w&&(f[i].events&POLLOUT)&&(v&(POLLOUT|POLLERR))){FD_SET(fd,w);ready++;}
  if(e&&(v&POLLPRI)){FD_SET(fd,e);ready++;}
 }return ready;
}
/* Sockets use MSG_DONTWAIT so readiness races cannot strand a worker. Respect
 * caller nonblocking flags and SO_RCVTIMEO/SO_SNDTIMEO; no FDs closed cross-thread. */
static int socket_timeout(int fd,int opt){struct timeval tv={0};socklen_t len=sizeof(tv);if(getsockopt(fd,SOL_SOCKET,opt,&tv,&len))return -1;int64_t ms=(int64_t)tv.tv_sec*1000+(tv.tv_usec+999)/1000;return ms?(ms>INT_MAX?INT_MAX:(int)ms):-1;}
static int wait_io(int fd,short events,int64_t end){
 if(io_deadline&&(!end||io_deadline<end))end=io_deadline;
 int timeout=-1;
 if(end){int64_t left=end-mono_ns();if(left<=0){errno=EAGAIN;return -1;}timeout=(int)((left+999999)/1000000);}
 struct pollfd p={.fd=fd,.events=events};int r=ya_poll(&p,1,timeout);
 if(r==0){errno=EAGAIN;return -1;}if(r<0)return -1;
 if(p.revents&POLLNVAL){errno=EBADF;return -1;}return 0;
}
ssize_t ya_recvfrom(int fd,void*b,size_t n,int flags,struct sockaddr*a,socklen_t*l){
 ya_thread_testcancel();if(flags&MSG_WAITALL){errno=ENOTSUP;return -1;}
 int nonblock=(flags&MSG_DONTWAIT)||(fcntl(fd,F_GETFL)&O_NONBLOCK);
 int tm=socket_timeout(fd,SO_RCVTIMEO);int64_t end=tm<0?0:mono_ns()+(int64_t)tm*1000000;
 for(;;){if(io_deadline&&mono_ns()>=io_deadline){errno=ETIMEDOUT;return -1;}ssize_t rc=recvfrom(fd,b,n,flags|MSG_DONTWAIT,a,l);if(rc>=0)return rc;
  if(errno==EINTR){ya_thread_testcancel();continue;}
  if((errno!=EAGAIN&&errno!=EWOULDBLOCK)||nonblock)return -1;
  if(wait_io(fd,POLLIN,end))return -1;
 }
}
ssize_t ya_recv(int fd,void*b,size_t n,int flags){return ya_recvfrom(fd,b,n,flags,NULL,NULL);}
ssize_t ya_sendto(int fd,const void*b,size_t n,int flags,const struct sockaddr*a,socklen_t l){
 ya_thread_testcancel();int nonblock=(flags&MSG_DONTWAIT)||(fcntl(fd,F_GETFL)&O_NONBLOCK);
 int tm=socket_timeout(fd,SO_SNDTIMEO);
 /* Bound outbound writes, including cleanup and unresponsive peers. */
 if(tm<0||tm>1000)tm=1000;
 int64_t end=mono_ns()+(int64_t)tm*1000000;
 for(;;){if(io_deadline&&mono_ns()>=io_deadline){errno=ETIMEDOUT;return -1;}ssize_t rc=sendto(fd,b,n,flags|MSG_DONTWAIT|MSG_NOSIGNAL,a,l);if(rc>=0)return rc;
  if(errno==EINTR){ya_thread_testcancel();continue;}
  if((errno!=EAGAIN&&errno!=EWOULDBLOCK)||nonblock)return -1;
  if(wait_io(fd,POLLOUT,end))return -1;
 }
}
ssize_t ya_send(int fd,const void*b,size_t n,int flags){return ya_sendto(fd,b,n,flags,NULL,0);}
static int is_socket(int fd){int t;socklen_t n=sizeof(t);return getsockopt(fd,SOL_SOCKET,SO_TYPE,&t,&n)==0;}
ssize_t ya_read(int fd,void*b,size_t n){if(is_socket(fd))return ya_recv(fd,b,n,0);ya_thread_testcancel();return read(fd,b,n);}
ssize_t ya_write(int fd,const void*b,size_t n){if(is_socket(fd))return ya_send(fd,b,n,0);ya_thread_testcancel();return write(fd,b,n);}
int ya_accept(int fd,struct sockaddr*a,socklen_t*l){
 /* Listening sockets are private to one receiver. Keep them nonblocking;
  * accepted sockets remain blocking as expected by recv/read wrappers. */
 int flags=fcntl(fd,F_GETFL);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)return -1;
 for(;;){ya_thread_testcancel();int c=accept(fd,a,l);if(c>=0)return c;
  if(errno==EINTR)continue;
  if(errno!=EAGAIN&&errno!=EWOULDBLOCK)return -1;
  if(wait_io(fd,POLLIN,0))return -1;
 }
}
int ya_nanosleep(const struct timespec*t,struct timespec*rem){
 if(!t||t->tv_sec<0||t->tv_nsec<0||t->tv_nsec>=1000000000){errno=EINVAL;return -1;}
 if(!self||self->disabled||self->exiting)return nanosleep(t,rem);
 int64_t end=mono_ns()+(int64_t)t->tv_sec*1000000000+t->tv_nsec;
 while(1){int64_t left=end-mono_ns();if(left<=0)break;int64_t ms=(left+999999)/1000000;int r=ya_poll(NULL,0,ms>INT_MAX?INT_MAX:(int)ms);if(r<0&&errno!=EINTR){if(rem){left=end-mono_ns();if(left<0)left=0;rem->tv_sec=left/1000000000;rem->tv_nsec=left%1000000000;}return -1;}}
 ya_thread_testcancel();return 0;
}
int ya_usleep(useconds_t us){struct timespec t={us/1000000,(us%1000000)*1000};return ya_nanosleep(&t,NULL);}
/* Selected SPS condvars are all default CLOCK_REALTIME. Check every 20ms,
 * always reacquiring the caller's mutex before executing cleanup handlers.
 * pthread_cond_timedwait is deliberately the real libc operation here. */
static int condwait(pthread_cond_t*c,pthread_mutex_t*m,const struct timespec*end){
 for(;;){ya_thread_testcancel();struct timespec t;clock_gettime(CLOCK_REALTIME,&t);t.tv_nsec+=20000000;if(t.tv_nsec>=1000000000){t.tv_sec++;t.tv_nsec-=1000000000;}
  if(end&&(end->tv_nsec<0||end->tv_nsec>=1000000000))return EINVAL;
  if(end&&(end->tv_sec<t.tv_sec||(end->tv_sec==t.tv_sec&&end->tv_nsec<t.tv_nsec)))t=*end;
  int r=pthread_cond_timedwait(c,m,&t);ya_thread_testcancel();if(r!=ETIMEDOUT)return r;
  if(end){struct timespec now;clock_gettime(CLOCK_REALTIME,&now);if(now.tv_sec>end->tv_sec||(now.tv_sec==end->tv_sec&&now.tv_nsec>=end->tv_nsec))return ETIMEDOUT;}
 }
}
int ya_cond_wait(pthread_cond_t*c,pthread_mutex_t*m){return condwait(c,m,NULL);}
int ya_cond_timedwait(pthread_cond_t*c,pthread_mutex_t*m,const struct timespec*t){return condwait(c,m,t);}
