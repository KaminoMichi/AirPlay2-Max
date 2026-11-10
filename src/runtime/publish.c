/* SPDX-License-Identifier: MIT
 * Atomic no-clobber publication to an existing Hybrid Root queue. No fallback
 * to check-then-rename or link/unlink (rootd requires st_nlink == 1).
 */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif
#ifndef YAP_PREFIX
#define YAP_PREFIX "/data/local/tmp/ya-airplay-root-"
#endif
#ifndef YAP_TARGET
#define YAP_TARGET "/data/local/tmp/rootcmd"
#endif
int main(int argc,char **argv){
 const char *prefix=YAP_PREFIX;
 if(argc!=2||strncmp(argv[1],prefix,strlen(prefix)))return 64;
 const char *s=argv[1]+strlen(prefix);for(int k=0;k<16;k++)if(!((s[k]>='0'&&s[k]<='9')||(s[k]>='a'&&s[k]<='f')))return 64;
 if(strcmp(s+16,"/request.part"))return 64;
 char dir[PATH_MAX];size_t n=strlen(argv[1])-strlen("/request.part");if(n>=sizeof dir)return 64;memcpy(dir,argv[1],n);dir[n]=0;
 char canonical[PATH_MAX];if(!realpath(dir,canonical)||strcmp(canonical,dir))return 65;
 struct stat d,st,at;if(lstat(dir,&d)||!S_ISDIR(d.st_mode)||d.st_uid!=getuid()||(d.st_mode&077))return 77;
 int fd=open(argv[1],O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return 65;
 if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||st.st_nlink!=1||(st.st_mode&077)||st.st_size<1||st.st_size>7000||fsync(fd)||lstat(argv[1],&at)||at.st_dev!=st.st_dev||at.st_ino!=st.st_ino){close(fd);return 65;}
 int result=(int)syscall(SYS_renameat2,AT_FDCWD,argv[1],AT_FDCWD,YAP_TARGET,RENAME_NOREPLACE);int e=errno;close(fd);
 if(result){fprintf(stderr,"Atomic root publication refused errno=%d; no overwrite/no retry\n",e);return e==EEXIST?75:69;}
 puts("PUBLISHED_ONCE_ATOMIC_NO_REPLACE");return 0;
}
