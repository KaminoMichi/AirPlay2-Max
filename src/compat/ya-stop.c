#include "ya-stop.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <time.h>
#include <limits.h>
int ya_stop_init(ya_stop *s) {
 atomic_init(&s->requested,0);
 s->wake_fd=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
 return s->wake_fd<0?-1:0;
}
int ya_stop_request(ya_stop *s) {
 atomic_store_explicit(&s->requested,1,memory_order_release);
 uint64_t one=1; ssize_t n;
 do {n=write(s->wake_fd,&one,sizeof(one));} while(n<0&&errno==EINTR);
 return n==sizeof(one)||(n<0&&errno==EAGAIN)?0:-1;
}
void ya_stop_destroy(ya_stop *s) { if(s->wake_fd>=0) close(s->wake_fd); s->wake_fd=-1; }
static int64_t now_ms(void) {
 struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t)) return -1;
 return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
int ya_stop_wait(ya_stop *s,struct pollfd *fds,unsigned long n,int timeout) {
 if(n>1024 || timeout < -1) {errno=EINVAL;return -1;}
 struct pollfd p[1025];
 for(unsigned long i=0;i<n;i++) {p[i]=fds[i];fds[i].revents=0;}
 p[n]=(struct pollfd){.fd=s->wake_fd,.events=POLLIN};
 int64_t start=now_ms(); if(start<0) return -1;
 int64_t end=timeout<0?0:start+timeout;
 for(;;) {
  if(atomic_load_explicit(&s->requested,memory_order_acquire)) {errno=ECANCELED;return -1;}
  int wait=-1;
  if(timeout>=0) {
   int64_t now=now_ms();if(now<0)return -1;
   int64_t rem=end-now; wait=rem<=0?0:(rem>INT_MAX?INT_MAX:(int)rem);
  }
  int r=poll(p,n+1,wait);
  if(r<0&&errno==EINTR) continue;
  if(atomic_load_explicit(&s->requested,memory_order_acquire)) {errno=ECANCELED;return -1;}
  if(r<=0)return r;
  if(p[n].revents) {errno=EIO;return -1;}
  for(unsigned long i=0;i<n;i++) fds[i].revents=p[i].revents;
  return r;
 }
}
ssize_t ya_stop_recv(ya_stop *s,int fd,void *buf,size_t len,int flags) {
 if(flags & MSG_WAITALL) {errno=ENOTSUP;return -1;}
 for(;;) {
  struct pollfd p={.fd=fd,.events=POLLIN};
  if(ya_stop_wait(s,&p,1,-1)<0)return -1;
  ssize_t n=recv(fd,buf,len,flags|MSG_DONTWAIT);
  if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)) continue;
  return n;
 }
}
