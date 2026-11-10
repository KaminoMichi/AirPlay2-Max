#define _GNU_SOURCE
#include "mdns.h"
#include "common.h"
#include <sys/un.h>
#include <stddef.h>
#ifndef YA_NSD_SOCKET_NAME
#define YA_NSD_SOCKET_NAME "ya-airplay-nsd-v1"
#endif
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* Persistent authenticated local stream; the Service unpublishes on EOF.
 * Only public Bonjour records cross this bridge, no pairing private keys.
 */
static int bridge=-1;
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
static char *raop_name,*airplay_name;static int service_port;
static int64_t nowms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int transfer(int fd,void *buf,size_t n,int output,int64_t end){
 size_t done=0;while(done<n){
  int64_t left=end-nowms();if(left<=0)return -1;
  struct pollfd p={.fd=fd,.events=output?POLLOUT:POLLIN};
  int r=poll(&p,1,(int)left);if(r<0&&errno==EINTR)continue;if(r<=0)return -1;
  ssize_t got=output?send(fd,(char*)buf+done,n-done,MSG_DONTWAIT|MSG_NOSIGNAL):recv(fd,(char*)buf+done,n-done,MSG_DONTWAIT);
  if(got<0&&(errno==EINTR||errno==EAGAIN))continue;if(got<=0)return -1;done+=(size_t)got;
 }return 0;
}
static void discard(void){if(bridge>=0){close(bridge);bridge=-1;}}
static int connect_bridge(void){
 const char *uidtext=getenv("YA_NSD_UID");char *tail=NULL;
 if(!uidtext||!*uidtext)return -1;
 unsigned long expected=strtoul(uidtext,&tail,10);if(*tail||expected<10000||expected>INT32_MAX)return -1;
 int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return -1;
 struct sockaddr_un a={.sun_family=AF_UNIX};const char name[]=YA_NSD_SOCKET_NAME;memcpy(a.sun_path+1,name,sizeof(name)-1);
 int rc=connect(fd,(struct sockaddr*)&a,offsetof(struct sockaddr_un,sun_path)+sizeof(name));
 if(rc<0&&errno==EINPROGRESS){struct pollfd p={fd,POLLOUT,0};if(poll(&p,1,1000)>0){int error=0;socklen_t n=sizeof(error);if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&n)==0&&error==0)rc=0;}}
 struct ucred cred;socklen_t n=sizeof(cred);
 if(rc<0||getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&n)||cred.uid!=(uid_t)expected){close(fd);return -1;}
 return fd;
}
static int put16(unsigned char*b,size_t*p,unsigned v){if(*p+2>32768||v>65535)return -1;b[(*p)++]=v>>8;b[(*p)++]=v;return 0;}
static int str(unsigned char*b,size_t*p,const char*s){if(!s)return -1;size_t n=strlen(s);if(n>255||put16(b,p,n)||*p+n>32768)return -1;memcpy(b+*p,s,n);*p+=n;return 0;}
static int records(unsigned char*b,size_t*p,char**txt){unsigned count=0;if(!txt)return -1;while(txt[count]){if(++count>128)return -1;}if(put16(b,p,count))return -1;for(unsigned i=0;i<count;i++)if(str(b,p,txt[i]))return -1;return 0;}
static int transact(unsigned char op,char**txt,char**secondary){
 unsigned char b[32768];size_t p=0;memcpy(b,"YNS1",4);p=4;b[p++]=op;
 if(op!='Q'&&(put16(b,&p,service_port)||str(b,&p,raop_name)||str(b,&p,airplay_name)||records(b,&p,txt)||records(b,&p,secondary)))return -1;
 uint32_t len=htonl((uint32_t)p),status=0;int64_t end=nowms()+6000;
 if(transfer(bridge,&len,4,1,end)||transfer(bridge,b,p,1,end)||transfer(bridge,&status,4,0,end)||ntohl(status)!=0){discard();return -1;}return 0;
}
static int registration(char*raop,char*airplay,int port,char**txt,char**secondary){
 int old;pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&old);pthread_mutex_lock(&guard);
 discard();free(raop_name);free(airplay_name);raop_name=strdup(raop);airplay_name=strdup(airplay);service_port=port;
 int rc=-1;if(raop_name&&airplay_name&&(bridge=connect_bridge())>=0)rc=transact('R',txt,secondary);
 pthread_mutex_unlock(&guard);pthread_setcancelstate(old,NULL);return rc;
}
static int update(char**txt,char**secondary){
 int old;pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&old);pthread_mutex_lock(&guard);
 int rc=bridge<0?-1:transact('U',txt,secondary);
 pthread_mutex_unlock(&guard);pthread_setcancelstate(old,NULL);return rc;
}
static void unregistration(void){
 int old;pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&old);pthread_mutex_lock(&guard);
 if(bridge>=0)transact('Q',NULL,NULL);discard();free(raop_name);free(airplay_name);raop_name=airplay_name=NULL;
 pthread_mutex_unlock(&guard);pthread_setcancelstate(old,NULL);
}
mdns_backend mdns_android_nsd={.name="android-nsd",.mdns_register=registration,.mdns_update=update,.mdns_unregister=unregistration};
