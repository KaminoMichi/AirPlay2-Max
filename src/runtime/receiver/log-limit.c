#define _GNU_SOURCE
#include "log-limit.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#ifndef YA_RECEIVER_FILE_LIMIT
#define YA_RECEIVER_FILE_LIMIT (8ULL*1024*1024)
#endif
int ya_log_limit_init(void){
  struct rlimit limit;if(getrlimit(RLIMIT_FSIZE,&limit))return errno;
  rlim_t cap=(rlim_t)YA_RECEIVER_FILE_LIMIT;
  if(limit.rlim_cur!=RLIM_INFINITY&&limit.rlim_cur<cap)cap=limit.rlim_cur;
  if(limit.rlim_max!=RLIM_INFINITY&&limit.rlim_max<cap)cap=limit.rlim_max;
  struct sigaction action={0};action.sa_handler=SIG_IGN;sigemptyset(&action.sa_mask);
  if(sigaction(SIGXFSZ,&action,NULL))return errno;
  limit.rlim_cur=cap;limit.rlim_max=cap;
  if(setrlimit(RLIMIT_FSIZE,&limit))return errno;
  fprintf(stderr,"YA_LOG status=bounded bytes=%llu scope=per_regular_file\n",(unsigned long long)cap);
  return 0;
}
