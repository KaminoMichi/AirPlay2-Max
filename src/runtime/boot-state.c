/* SPDX-License-Identifier: MIT
 * Reset only this product's stale runtime objects after a verified boot-id
 * change. Never signal a recorded PID and never repair same-boot crashes.
 */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef YAB_ROOT
#define YAB_ROOT "/data/local/tmp/ya-airplay"
#endif
#ifndef YAB_BOOT_ID
#define YAB_BOOT_ID "/proc/sys/kernel/random/boot_id"
#endif
static const char *files[]={"active","status.json","clean-stop","active-logdir","control.sock","remote-test.sock","shm-nqptp","companion.claim","shairport.conf","voice.active","voice.stop","voice.txt",NULL};
static int uuid(const char *s){
 if(strlen(s)!=37||s[36]!='\n')return 0;
 for(int k=0;k<36;k++){if(k==8||k==13||k==18||k==23){if(s[k]!='-')return 0;}else if(!((s[k]>='0'&&s[k]<='9')||(s[k]>='a'&&s[k]<='f')))return 0;}return 1;
}
static int get_text(int d,const char *p,char *out,size_t size,int require_owned){
 int f=openat(d,p,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(f<0)return errno==ENOENT?1:-1;
 struct stat st;int rc=-1;
 if(fstat(f,&st)||!S_ISREG(st.st_mode)||(require_owned&&(st.st_uid!=0||st.st_nlink!=1||(st.st_mode&077))))goto done;
 ssize_t n=read(f,out,size-1);if(n<0||(size_t)n>=size-1)goto done;out[n]=0;rc=0;
done:close(f);return rc;
}
static int prepare(void){
 if(getuid()!=0)return 77;
 struct stat st;if(lstat(YAB_ROOT,&st)||!S_ISDIR(st.st_mode)||st.st_uid||(st.st_mode&077))return 77;
 int d=open(YAB_ROOT,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(d<0)return 69;
 char marker[64];if(get_text(d,"PRODUCT",marker,sizeof marker,1)||strcmp(marker,"YA_AIRPLAY_PUBLIC_V1\n")){close(d);return 77;}
 int r=openat(d,"run",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);close(d);if(r<0)return 69;
 int lock=-1,guard=-1,rc=69;char current[64],old[64];
 if(fstat(r,&st)||st.st_uid||(st.st_mode&077))goto done;
 lock=openat(r,"boot-state.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
 if(lock<0||fstat(lock,&st)||st.st_uid||!S_ISREG(st.st_mode)||st.st_nlink!=1||flock(lock,LOCK_EX|LOCK_NB))goto done;
 if(get_text(AT_FDCWD,YAB_BOOT_ID,current,sizeof current,0)||!uuid(current))goto done;
 int prior=get_text(r,"boot-id",old,sizeof old,1);if(prior<0||(!prior&&!uuid(old)))goto done;
 if(!prior&&!strcmp(current,old)){puts("YA_BOOT state=same_boot no_reset=true");rc=0;goto done;}
 /* The native owner holds this lock for its entire lifetime. */
 guard=openat(r,"supervisor.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
 if(guard<0||fstat(guard,&st)||st.st_uid||!S_ISREG(st.st_mode)||st.st_nlink!=1||flock(guard,LOCK_EX|LOCK_NB))goto done;
 /* Validate every object before the first unlink; missing prior stamp means
  * unknown provenance, so existing runtime objects must not be adopted. */
 for(unsigned k=0;files[k];k++){
  if(fstatat(r,files[k],&st,AT_SYMLINK_NOFOLLOW)){if(errno==ENOENT)continue;goto done;}
  int sock=!strcmp(files[k],"control.sock")||!strcmp(files[k],"remote-test.sock");
  if(prior||st.st_uid||st.st_nlink!=1||(st.st_mode&077)||(sock?!S_ISSOCK(st.st_mode):!S_ISREG(st.st_mode)))goto done;
 }
 int f=openat(r,".boot-id.next",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(f<0)goto done;
 if(write(f,current,37)!=37||fsync(f)){close(f);unlinkat(r,".boot-id.next",0);goto done;}close(f);
 for(unsigned k=0;files[k];k++)if(unlinkat(r,files[k],0)&&errno!=ENOENT){unlinkat(r,".boot-id.next",0);goto done;}
 if(renameat(r,".boot-id.next",r,"boot-id")||fsync(r))goto done;
 puts(prior?"YA_BOOT state=initialized":"YA_BOOT state=new_boot stale_runtime_removed=true");rc=0;
done:
 if(guard>=0)close(guard);if(lock>=0)close(lock);close(r);return rc;
}
int main(int argc,char **argv){if(argc!=2||strcmp(argv[1],"prepare"))return 64;umask(077);return prepare();}
