#define _GNU_SOURCE
#include "remote-io.h"
#include <stdio.h>
#include <fcntl.h>
static int exit_status(unsigned s){switch(s){case YR_ACK:return 0;case YR_NO_SESSION:return 69;case YR_BUSY:case YR_STALE:case YR_STOPPING:return 75;case YR_INVALID:return 64;case YR_DENIED:return 77;case YR_REJECTED:return 76;default:return 70;}}
int main(int argc,char**argv){
 if(argc!=2){fprintf(stderr,"Usage: ya-airplay-remote play|pause|next|previous\n");return 64;}
 unsigned cmd=99;for(unsigned k=0;k<6;k++)if(yr_command_valid(k)&&!strcmp(argv[1],yr_command(k)))cmd=k;
 if(cmd==99){fprintf(stderr,"Invalid command\n");return 64;}
 int64_t began=yr_now();int committed=0,code=70,fd=-1;const char*why="not_sent";
 if(getuid()!=0||yr_private_directory()){fprintf(stderr,"YA_REMOTE status=denied action=UNKNOWN\n");return 77;}
 struct stat st;if(lstat(YR_SOCKET,&st)||!S_ISSOCK(st.st_mode)||st.st_uid!=0||(st.st_mode&077)){fprintf(stderr,"YA_REMOTE status=unavailable action=UNKNOWN\n");return 69;}
 fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)goto finish;
 struct sockaddr_un a=yr_address();if(connect(fd,(void*)&a,sizeof(a)))goto finish; // full backlog => refuse, no retry
 pid_t pid;if(yr_root_peer(fd,&pid)){code=77;why="denied";goto finish;}
 char proc[64],path[PATH_MAX];snprintf(proc,sizeof(proc),"/proc/%d/exe",pid);ssize_t n=readlink(proc,path,sizeof(path)-1);if(n<0){why="receiver_gone";goto finish;}path[n]=0;
 if(strcmp(path,YR_RECEIVER)){code=77;why="wrong_receiver";goto finish;}
 yr_packet p={.magic=YR_MAGIC,.version=YR_VERSION,.type=YR_HELLO,.command=cmd,.started=(uint64_t)began};
 if(yr_packet_io(fd,&p,1,began+200000000)||yr_packet_io(fd,&p,0,began+600000000))goto finish;
 if(p.magic!=YR_MAGIC||p.version!=YR_VERSION||p.command!=cmd||p.started!=(uint64_t)began){why="protocol";goto finish;}
 if(p.type==YR_RESULT){code=exit_status(p.status);why=yr_status(p.status);goto finish;}
 if(p.type!=YR_READY||p.status||!p.cid||!p.epoch||!p.lease){why="protocol";goto finish;}
 yr_packet expected=p;p.type=YR_COMMIT;
 committed=1; // From first send attempt onward, lost reply is uncertain; never resend.
 if(yr_packet_io(fd,&p,1,began+700000000)||yr_packet_io(fd,&p,0,began+1300000000))goto finish;
 expected.type=YR_RESULT;expected.status=p.status;
 if(memcmp(&p,&expected,sizeof(p))){why="protocol";goto finish;}
 code=exit_status(p.status);why=yr_status(p.status);
finish:
 if(fd>=0)close(fd);
 printf("YA_REMOTE command=%s status=%s exit=%d action=UNKNOWN retry=never\n",yr_command(cmd),committed&&code==70?"uncertain":why,code);
 return code;
}
