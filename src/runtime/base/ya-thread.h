#pragma once
/* Deferred cooperative cancellation for THIS audited SPS build, not a Bionic ABI replacement. */
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#ifndef PTHREAD_CANCEL_ENABLE
#define PTHREAD_CANCEL_ENABLE 0
#define PTHREAD_CANCEL_DISABLE 1
#endif
#ifndef PTHREAD_CANCELED
#define PTHREAD_CANCELED ((void *)-1)
#endif
int ya_thread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int ya_thread_cancel(pthread_t);
int ya_thread_join(pthread_t,void **);
int ya_thread_setcancelstate(int,int *);
void ya_thread_testcancel(void);
size_t ya_thread_live_count(void);
int ya_poll(struct pollfd *,nfds_t,int);
int ya_select(int,fd_set *,fd_set *,fd_set *,struct timeval *);
ssize_t ya_recv(int,void *,size_t,int);
ssize_t ya_recvfrom(int,void *,size_t,int,struct sockaddr *,socklen_t *);
ssize_t ya_read(int,void *,size_t);
ssize_t ya_write(int,const void *,size_t);
ssize_t ya_send(int,const void *,size_t,int);
ssize_t ya_sendto(int,const void *,size_t,int,const struct sockaddr *,socklen_t);
int ya_accept(int,struct sockaddr *,socklen_t *);
int ya_nanosleep(const struct timespec *,struct timespec *);
int ya_usleep(useconds_t);
int ya_cond_wait(pthread_cond_t *,pthread_mutex_t *);
int ya_cond_timedwait(pthread_cond_t *,pthread_mutex_t *,const struct timespec *);
#ifndef YA_THREAD_IMPLEMENTATION
#define pthread_create ya_thread_create
#define pthread_cancel ya_thread_cancel
#define pthread_join ya_thread_join
#define pthread_setcancelstate ya_thread_setcancelstate
#define pthread_testcancel ya_thread_testcancel
#define poll ya_poll
#define select ya_select
#define recv ya_recv
#define recvfrom ya_recvfrom
#define read ya_read
#define write ya_write
#define send ya_send
#define sendto ya_sendto
#define accept ya_accept
#define nanosleep ya_nanosleep
#define usleep ya_usleep
#define pthread_cond_wait ya_cond_wait
#define pthread_cond_timedwait ya_cond_timedwait
#endif
