#define main supervisor_entry
#include "../src/runtime/supervisor.c"
#undef main
#include <assert.h>
int main(int argc,char **argv){
 assert(argc==2);assert(!bound_logs());struct rlimit r;assert(!getrlimit(RLIMIT_FSIZE,&r));assert(r.rlim_cur<=8*1024*1024&&r.rlim_max==r.rlim_cur);
 int f=open(argv[1],O_WRONLY|O_CREAT|O_EXCL,0600);assert(f>=0);char data[8192]={0};ssize_t n;
 while((n=write(f,data,sizeof data))>0){}assert(n<0&&errno==EFBIG);struct stat s;assert(!fstat(f,&s)&&s.st_size==(off_t)r.rlim_cur);close(f);
 puts("PASS regular logs bounded without SIGXFSZ termination");return 0;
}
