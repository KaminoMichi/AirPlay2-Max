#pragma once
#include "remote-protocol.h"
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
static int64_t yr_now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (int64_t)t.tv_sec*1000000000+t.tv_nsec;}
/* Deadline covers readiness races, partial/oversized datagrams and EINTR.
 * MSG_TRUNC makes an oversized SEQPACKET fail rather than accept its prefix. */
static int yr_packet_io(int fd,yr_packet*p,int sending,int64_t end){
 while(yr_now()<end){
  struct pollfd f={.fd=fd,.events=sending?POLLOUT:POLLIN};int r=poll(&f,1,10);
  if(r<0){if(errno==EINTR)continue;return -1;}if(!r)continue;if(f.revents&(POLLERR|POLLNVAL))return -1;
  ssize_t n=sending?send(fd,p,sizeof(*p),MSG_NOSIGNAL|MSG_DONTWAIT):recv(fd,p,sizeof(*p),MSG_DONTWAIT|MSG_TRUNC);
  if(n==(ssize_t)sizeof(*p))return 0;
  if(n<0&&(errno==EAGAIN||errno==EINTR))continue;
  errno=EPROTO;return -1;
 }errno=ETIMEDOUT;return -1;
}
static int yr_private_directory(void){
 char resolved[PATH_MAX];struct stat st;
 if(!realpath(YR_ROOT "/run",resolved)||strcmp(resolved,YR_ROOT "/run"))return -1;
 if(stat(resolved,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=0||(st.st_mode&077))return -1;
 return 0;
}
static int yr_root_peer(int fd,pid_t*pid){struct ucred c;socklen_t n=sizeof(c);if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&c,&n)||n!=sizeof(c)||c.uid!=0||c.pid<=0)return -1;if(pid)*pid=c.pid;return 0;}
static struct sockaddr_un yr_address(void){struct sockaddr_un a={.sun_family=AF_UNIX};_Static_assert(sizeof(YR_SOCKET)<=sizeof(a.sun_path),"socket path too long");memcpy(a.sun_path,YR_SOCKET,sizeof(YR_SOCKET));return a;}
