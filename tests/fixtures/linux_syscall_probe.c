/* S3 系统调用探针：逐项验证 35 项子集（含 getdents64）与拒绝策略。
 * 通过时打印 "LINUX SYSCALL PROBE PASSED"；任一失败以非零退出。 */
#include "linux_raw.h"

static void check(int *failures,int condition,const char *name){
 if(condition)return;
 lx_puts("PROBE FAIL ");
 lx_puts(name);
 lx_puts("\n");
 (*failures)++;
}

static int directory_has(void *buffer,long bytes,const char *want){
 long offset=0;
 while(offset<bytes){
  LxDirent64 *entry=(LxDirent64*)((char*)buffer+offset);
  if(!entry->d_reclen)return 0;
  if(lx_same((char*)buffer+offset+LX_DIRENT_NAME_OFF,want))return 1;
  offset+=entry->d_reclen;
 }
 return 0;
}

static int probe(void){
 int failures=0;
 char buffer[512];

 /* 文件类：openat/read/write/fstat/lseek/newfstatat/close/getdents64 */
 long fd=lx_raw(LX_SYS_OPENAT,LX_AT_FDCWD,(long)"etc/readme.txt",LX_O_RDONLY,0,0,0);
 check(&failures,fd>=3,"openat");
 if(fd>=3){
  LxStat st;
  long r=lx_raw(LX_SYS_FSTAT,fd,(long)&st,0,0,0,0);
  /* 磁盘内容由其它用例追加，探针只校验大小在 fstat/newfstatat/lseek 间一致。 */
  uint64_t size=st.st_size;
  check(&failures,r==0&&size>0&&(st.st_mode&LX_S_IFMT)==LX_S_IFREG,"fstat");
  r=lx_raw(LX_SYS_LSEEK,fd,0,LX_SEEK_SET,0,0,0);
  check(&failures,r==0,"lseek set");
  r=lx_raw(LX_SYS_READ,fd,(long)buffer,8,0,0,0);
  check(&failures,r==8,"read");
  r=lx_raw(LX_SYS_NEWFSTATAT,LX_AT_FDCWD,(long)"etc/readme.txt",(long)&st,0,0,0);
  check(&failures,r==0&&st.st_size==size,"newfstatat");
  r=lx_raw(LX_SYS_LSEEK,fd,0,LX_SEEK_END,0,0,0);
  check(&failures,r==(long)size,"lseek end");
  r=lx_raw(LX_SYS_CLOSE,fd,0,0,0,0,0);
  check(&failures,r==0,"close");
 }
 fd=lx_raw(LX_SYS_OPENAT,LX_AT_FDCWD,(long)"etc",LX_O_RDONLY|LX_O_DIRECTORY,0,0,0);
 check(&failures,fd>=3,"openat dir");
 if(fd>=3){
  long r=lx_raw(LX_SYS_GETDENTS64,fd,(long)buffer,sizeof(buffer),0,0,0);
  check(&failures,r>0&&directory_has(buffer,r,"readme.txt"),"getdents64");
  lx_raw(LX_SYS_CLOSE,fd,0,0,0,0,0);
 }

 /* 内存类：brk/mmap/munmap/mprotect */
 long base=lx_raw(LX_SYS_BRK,0,0,0,0,0,0);
 check(&failures,base>0,"brk query");
 long grown=lx_raw(LX_SYS_BRK,base+4096,0,0,0,0,0);
 check(&failures,grown==base+4096,"brk grow");
 long refused=lx_raw(LX_SYS_BRK,base+(64l<<20),0,0,0,0,0);
 check(&failures,refused==grown,"brk bound");
 long mapping=lx_raw(LX_SYS_MMAP,0,4096,LX_PROT_READ|LX_PROT_WRITE,
                     LX_MAP_PRIVATE|LX_MAP_ANONYMOUS,-1,0);
 check(&failures,mapping>0,"mmap");
 if(mapping>0){
  *(volatile char*)(mapping+16)='x';
  check(&failures,*(volatile char*)(mapping+16)=='x',"mmap write");
  long r=lx_raw(LX_SYS_MPROTECT,mapping,4096,LX_PROT_READ|LX_PROT_WRITE,0,0,0);
  check(&failures,r==0,"mprotect rw");
  r=lx_raw(LX_SYS_MPROTECT,mapping,4096,LX_PROT_READ|LX_PROT_EXEC,0,0,0);
  check(&failures,r==-LX_ENOSYS,"mprotect exec");
  r=lx_raw(LX_SYS_MUNMAP,mapping,4096,0,0,0,0);
  check(&failures,r==0,"munmap");
 }

 /* 时间/进程/身份 */
 LxTimespec time;
 long r=lx_raw(LX_SYS_CLOCK_GETTIME,0,(long)&time,0,0,0,0);
 check(&failures,r==0&&time.tv_nsec>=0&&time.tv_nsec<1000000000,"clock_gettime");
 LxTimeval elapsed;
 r=lx_raw(LX_SYS_GETTIMEOFDAY,(long)&elapsed,0,0,0,0,0);
 check(&failures,r==0&&elapsed.tv_usec<1000000,"gettimeofday");
 LxTimespec request={(uint64_t)0,(uint64_t)20000000},remaining;
 r=lx_raw(LX_SYS_NANOSLEEP,(long)&request,(long)&remaining,0,0,0,0);
 check(&failures,r==0,"nanosleep");
 long pid=lx_raw(LX_SYS_GETPID,0,0,0,0,0,0);
 long tid=lx_raw(LX_SYS_GETTID,0,0,0,0,0,0);
 check(&failures,pid>0&&tid==pid,"getpid/gettid");
 long uid=lx_raw(LX_SYS_GETUID,0,0,0,0,0,0);
 check(&failures,uid>=0,"getuid");
 r=lx_raw(LX_SYS_SET_TID_ADDRESS,(long)&failures,0,0,0,0,0);
 check(&failures,r==pid,"set_tid_address");
 LxUtsname uts;
 r=lx_raw(LX_SYS_UNAME,(long)&uts,0,0,0,0,0);
 check(&failures,r==0&&lx_same(uts.sysname,"Linux")&&lx_same(uts.machine,"x86_64"),"uname");
 LxSysinfo info;
 r=lx_raw(LX_SYS_SYSINFO,(long)&info,0,0,0,0,0);
 check(&failures,r==0&&info.totalram>0&&info.mem_unit>0,"sysinfo");
 LxRlimit limit;
 r=lx_raw(LX_SYS_PRLIMIT64,0,LX_RLIMIT_NOFILE,0,(long)&limit,0,0);
 check(&failures,r==0&&limit.cur==64,"prlimit64 query");
 r=lx_raw(LX_SYS_PRLIMIT64,0,LX_RLIMIT_NOFILE,(long)&limit,0,0,0);
 check(&failures,r==-LX_EPERM,"prlimit64 raise");

 /* ioctl/poll/ppoll */
 LxWinsize window;
 r=lx_raw(LX_SYS_IOCTL,1,LX_TIOCGWINSZ,(long)&window,0,0,0);
 check(&failures,r==0&&window.ws_col==80&&window.ws_row==25,"ioctl winsize");
 r=lx_raw(LX_SYS_IOCTL,1,0xdead,(long)&window,0,0,0);
 check(&failures,r==-LX_ENOTTY,"ioctl notty");
 LxPollfd poll_fd={1,LX_POLLOUT,0};
 r=lx_raw(LX_SYS_POLL,(long)&poll_fd,1,0,0,0,0);
 check(&failures,r==1&&(poll_fd.revents&LX_POLLOUT),"poll");
 poll_fd.revents=0;
 r=lx_raw(LX_SYS_PPOLL,(long)&poll_fd,1,0,0,0,0);
 check(&failures,r==1&&(poll_fd.revents&LX_POLLOUT),"ppoll");

 /* pipe2/dup/dup2/dup3/fcntl */
 int pair[2]={-1,-1};
 r=lx_raw(LX_SYS_PIPE2,(long)pair,0,0,0,0,0);
 check(&failures,r==0&&pair[0]>=3&&pair[1]>=3,"pipe2");
 if(r==0){
  char byte='P';
  check(&failures,lx_raw(LX_SYS_WRITE,pair[1],(long)&byte,1,0,0,0)==1,"pipe write");
  byte=0;
  check(&failures,lx_raw(LX_SYS_READ,pair[0],(long)&byte,1,0,0,0)==1&&byte=='P',"pipe read");
  lx_raw(LX_SYS_CLOSE,pair[0],0,0,0,0,0);
  lx_raw(LX_SYS_CLOSE,pair[1],0,0,0,0,0);
 }
 long duplicate=lx_raw(LX_SYS_DUP,1,0,0,0,0,0);
 check(&failures,duplicate>=3,"dup");
 check(&failures,lx_raw(LX_SYS_DUP2,duplicate,7,0,0,0,0)==7,"dup2");
 check(&failures,lx_raw(LX_SYS_DUP3,1,8,0,0,0,0)==8,"dup3");
 check(&failures,lx_raw(LX_SYS_FCNTL,7,LX_F_GETFD,0,0,0,0)==0,"fcntl");
 lx_raw(LX_SYS_CLOSE,duplicate,0,0,0,0,0);
 lx_raw(LX_SYS_CLOSE,7,0,0,0,0,0);
 lx_raw(LX_SYS_CLOSE,8,0,0,0,0,0);

 /* getcwd/readlinkat/sched_yield */
 r=lx_raw(LX_SYS_GETCWD,(long)buffer,sizeof(buffer),0,0,0,0);
 check(&failures,r==2&&buffer[0]=='/'&&buffer[1]==0,"getcwd");
 r=lx_raw(LX_SYS_GETCWD,(long)buffer,1,0,0,0,0);
 check(&failures,r==-LX_ERANGE,"getcwd erange");
 r=lx_raw(LX_SYS_READLINKAT,LX_AT_FDCWD,(long)"etc/readme.txt",(long)buffer,sizeof(buffer),0,0);
 check(&failures,r==-LX_EINVAL,"readlinkat");
 check(&failures,lx_raw(LX_SYS_SCHED_YIELD,0,0,0,0,0,0)==0,"sched_yield");

 /* 拒绝策略 */
 check(&failures,lx_raw(LX_SYS_GETRANDOM,(long)buffer,16,0,0,0,0)==-LX_ENOSYS,"getrandom");
 check(&failures,lx_raw(LX_SYS_ARCH_PRCTL,LX_ARCH_SET_FS,0,0,0,0,0)==-LX_ENOSYS,"arch_prctl fs");
 check(&failures,lx_raw(LX_SYS_ARCH_PRCTL,0xdead,0,0,0,0,0)==-LX_EINVAL,"arch_prctl invalid");
 check(&failures,lx_raw(999,0,0,0,0,0,0)==-LX_ENOSYS,"unknown syscall");
 check(&failures,lx_raw(LX_SYS_PTRACE,0,0,0,0,0,0)==-LX_EPERM,"ptrace denied");
 check(&failures,lx_raw(LX_SYS_MOUNT,0,0,0,0,0,0)==-LX_EPERM,"mount denied");
 check(&failures,lx_raw(LX_SYS_IO_URING_SETUP,0,0,0,0,0,0)==-LX_EPERM,"io_uring denied");
 check(&failures,lx_raw(LX_SYS_SETUID,0,0,0,0,0,0)==-LX_EPERM,"setuid denied");

 return failures;
}

long sf_tool_main(long argc,char **argv){
 (void)argc;(void)argv;
 int failures=probe();
 if(failures){
  lx_puts("LINUX SYSCALL PROBE FAILED\n");
  return 1;
 }
 lx_puts("LINUX SYSCALL PROBE PASSED\n");
 return 0;
}
