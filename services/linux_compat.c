/* Linux 兼容翻译器（S3）。
 *
 * 本文件编译为位置无关 blob：代码/只读数据由 sf-init 追加为 RX 段，静态数据
 * 追加为紧随其后的 RW 页。入口汇编先建立帧页、翻译器栈与 Linux 初始栈，注册
 * 兼容描述符后跳转 Linux 入口；每次 int 0x80 上行调用都会切到独立栈并进入
 * sf_linux_dispatch，用 int 0x81 访问原生系统调用。
 *
 * 有界性：fd 表 32 项、pipe 槽 4 个、brk 1 MiB、单次 read/write 分块受
 * SfMessage 4 KiB 限制；无动态内存分配。 */
#include "sf.h"
#include "sf_file.h"
#include "linux_abi.h"

int sf_linux_errno(int64_t status);
int64_t lx_sys(uint64_t number,uint64_t a,uint64_t b,uint64_t c);

/* ---- 局部字符串/内存原语（blob 不链接 libsf） ---- */
void *memcpy(void *dst,const void *src,size_t n){
 volatile uint8_t *d=(volatile uint8_t*)dst;const uint8_t *s=(const uint8_t*)src;
 for(size_t i=0;i<n;i++)d[i]=s[i];
 return dst;
}
void *memset(void *dst,int value,size_t n){
 volatile uint8_t *d=(volatile uint8_t*)dst;
 for(size_t i=0;i<n;i++)d[i]=(uint8_t)value;
 return dst;
}
int memcmp(const void *a,const void *b,size_t n){
 const uint8_t *x=(const uint8_t*)a,*y=(const uint8_t*)b;
 for(size_t i=0;i<n;i++)if(x[i]!=y[i])return(int)x[i]-(int)y[i];
 return 0;
}
size_t strlen(const char *s){size_t n=0;while(s[n])n++;return n;}
int strcmp(const char *a,const char *b){
 while(*a&&*a==*b){a++;b++;}
 return(int)(uint8_t)*a-(int)(uint8_t)*b;
}
int strncmp(const char *a,const char *b,size_t n){
 for(size_t i=0;i<n;i++){
  if(a[i]!=b[i])return(int)(uint8_t)a[i]-(int)(uint8_t)b[i];
  if(!a[i])return 0;
 }
 return 0;
}

/* 原生系统调用：int 0x81，仅兼容任务可用。 */
int64_t lx_sys(uint64_t number,uint64_t a,uint64_t b,uint64_t c){
 register uint64_t rax __asm__("rax")=number;
 register uint64_t rdi __asm__("rdi")=a;
 register uint64_t rsi __asm__("rsi")=b;
 register uint64_t rdx __asm__("rdx")=c;
 __asm__ volatile("int $0x81":"+a"(rax):"D"(rdi),"S"(rsi),"d"(rdx):"memory","cc");
 return(int64_t)rax;
}

/* ---- 状态 ---- */
enum { K_NONE=0,K_STDIN,K_CONSOLE,K_FILE,K_DIR,K_PIPE_R,K_PIPE_W };
#define LX_FD_MAX 32u
#define LX_PIPE_MAX 4u
#define LX_PIPE_BYTES 4096u
#define LX_BRK_BYTES 0x100000ull

typedef struct { uint8_t kind,reserved; uint32_t pipe; uint32_t index; uint64_t handle; } LxFd;
typedef struct { int used,readers,writers; uint8_t *buffer; uint32_t head,tail,bytes; } LxPipe;

static SfFileClient file_client;
static const SfBootArgs *boot_args;
static int file_ready,root_ready,console_ready;
static uint64_t root_handle;
static int console_handle;
static LxFd fds[LX_FD_MAX];
static LxPipe pipes[LX_PIPE_MAX];
static uint64_t brk_base,brk_cur,brk_end,tid_address;

static int64_t fail(int64_t status){return-(int64_t)sf_linux_errno(status);}

static int io_init(void){
 if(file_ready)return 0;
 const SfBootArgs *a=(const SfBootArgs*)(uintptr_t)SF_ARGS;
 if(!a||a->size!=sizeof(*a)||a->abi_major!=SF_ABI||!a->manager||!a->service)return-LX_EIO;
 if(sf_file_client_init(&file_client,a->manager,a->service,3000))return-LX_EIO;
 boot_args=a;
 fds[0].kind=K_STDIN;
 fds[1].kind=K_CONSOLE;
 fds[2].kind=K_CONSOLE;
 file_ready=1;
 return 0;
}

static int64_t get_root(void){
 if(root_ready)return(int64_t)root_handle;
 int64_t h=sf_file_root(&file_client);
 if(h<0)return h;
 root_handle=(uint64_t)h;root_ready=1;
 return h;
}

static int64_t get_console(void){
 if(console_ready)return console_handle;
 int64_t h=sf_file_console(&file_client);
 if(h<0)return h;
 console_handle=(int)h;console_ready=1;
 return h;
}

static const char *relative(const char *path){
 while(*path=='/')path++;
 return *path?path:".";
}

static int fd_alloc(void){
 for(unsigned i=3;i<LX_FD_MAX;i++)if(!fds[i].kind){
  fds[i]=(LxFd){0};return(int)i;
 }
 return-LX_EMFILE;
}

static int fd_valid(int fd){return fd>=0&&fd<(int)LX_FD_MAX&&fds[fd].kind;}

static int pipe_alloc(void){
 for(unsigned i=0;i<LX_PIPE_MAX;i++)if(!pipes[i].used){pipes[i]=(LxPipe){1,0,0,0,0,0,0};return(int)i;}
 return-LX_EMFILE;
}

/* 关闭一端只释放对应方向的引用；收尾时两端都归零才回收缓冲区。 */
static void pipe_put(int slot,int kind){
 if(slot<0||slot>=(int)LX_PIPE_MAX)return;
 LxPipe *p=&pipes[slot];
 if(kind==K_PIPE_R){if(p->readers>0)p->readers--;}
 else if(kind==K_PIPE_W){if(p->writers>0)p->writers--;}
 if(p->readers<=0&&p->writers<=0){
  if(p->buffer)lx_sys(SYS_MUNMAP,(uint64_t)p->buffer,LX_PIPE_BYTES,0);
  *p=(LxPipe){0,0,0,0,0,0,0};
 }
}

/* dup 后多个描述符共享同一个服务端句柄，只有最后一个关闭时才真正释放。 */
static void fd_hold(int fd){
 LxFd *e=&fds[fd];
 if(e->kind==K_PIPE_R){if(e->pipe<LX_PIPE_MAX)pipes[e->pipe].readers++;}
 else if(e->kind==K_PIPE_W){if(e->pipe<LX_PIPE_MAX)pipes[e->pipe].writers++;}
}

static int64_t fd_close(int fd){
 if(!fd_valid(fd))return-LX_EBADF;
 LxFd *e=&fds[fd];
 if(e->kind==K_PIPE_R||e->kind==K_PIPE_W){pipe_put((int)e->pipe,e->kind);e->kind=K_NONE;return 0;}
 if(e->kind==K_FILE||e->kind==K_DIR){
  for(unsigned i=0;i<LX_FD_MAX;i++)
   if((int)i!=fd&&(fds[i].kind==K_FILE||fds[i].kind==K_DIR)&&fds[i].handle==e->handle){
    e->kind=K_NONE;
    return 0;
   }
  int64_t r=sf_file_close(&file_client,e->handle);
  e->kind=K_NONE;
  return r<0?fail(r):0;
 }
 e->kind=K_NONE;
 return 0;
}

static int64_t fd_clone(int fd,int dst){
 if(!fd_valid(fd))return-LX_EBADF;
 if(dst<0||dst>=(int)LX_FD_MAX)return-LX_EBADF;
 if(dst==fd)return fd;
 if(fds[dst].kind){int64_t r=fd_close(dst);if(r<0)return r;}
 fds[dst]=fds[fd];
 fd_hold(fd);
 return dst;
}

/* ---- 文件类 ---- */
static int64_t lx_openat(int64_t dirfd,const char *path,int64_t flags,int64_t mode){
 (void)mode;
 if(!path||!*path)return-LX_ENOENT;
 if(flags&(LX_O_CREAT|LX_O_EXCL|LX_O_TRUNC|LX_O_APPEND))return-LX_EACCES;
 if(dirfd!=LX_AT_FDCWD&&!fd_valid((int)dirfd))return-LX_EBADF;
 const char *name=relative(path);
 uint32_t fmode=(flags&LX_O_ACCMODE)==LX_O_RDONLY?SF_OPEN_READ:SF_OPEN_WRITE;
 if(flags&LX_O_DIRECTORY)fmode|=SF_OPEN_DIRECTORY;
 int fd=fd_alloc();
 if(fd<0)return fd;
 if(!strcmp(name,"dev/console")){
  if(flags&LX_O_DIRECTORY)return-LX_ENOTDIR;
  int64_t h=get_console();
  if(h<0)return fail(h);
  fds[fd].kind=K_CONSOLE;fds[fd].handle=(uint64_t)h;
  return fd;
 }
 int64_t base=get_root();
 if(base<0)return fail(base);
 if(dirfd!=LX_AT_FDCWD)base=(int64_t)fds[dirfd].handle;
 int64_t h=sf_file_openat(&file_client,(uint64_t)base,name,fmode);
 if(h<0)return fail(h);
 fds[fd].kind=(flags&LX_O_DIRECTORY)?K_DIR:K_FILE;
 fds[fd].handle=(uint64_t)h;
 return fd;
}

static int64_t pipe_read(int slot,void *buffer,uint64_t count){
 if(slot<0||slot>=(int)LX_PIPE_MAX||!pipes[slot].used)return-LX_EBADF;
 LxPipe *p=&pipes[slot];
 if(!p->bytes)return p->writers>0?-LX_EAGAIN:0;
 uint8_t *out=(uint8_t*)buffer;uint64_t done=0;
 while(done<count&&p->bytes){
  out[done++]=p->buffer[p->head];
  p->head=(p->head+1)%LX_PIPE_BYTES;
  p->bytes--;
 }
 return(int64_t)done;
}

static int64_t pipe_write(int slot,const void *buffer,uint64_t count){
 if(slot<0||slot>=(int)LX_PIPE_MAX||!pipes[slot].used)return-LX_EBADF;
 LxPipe *p=&pipes[slot];
 if(p->readers<=0)return-LX_EPIPE;
 if(!p->buffer)return-LX_EBADF;
 const uint8_t *in=(const uint8_t*)buffer;uint64_t done=0;
 while(done<count&&p->bytes<LX_PIPE_BYTES){
  p->buffer[p->tail]=in[done++];
  p->tail=(p->tail+1)%LX_PIPE_BYTES;
  p->bytes++;
 }
 return done?(int64_t)done:-LX_EAGAIN;
}

static int64_t lx_read(int fd,void *buffer,uint64_t count){
 if(!count)return 0;
 if(!buffer)return-LX_EFAULT;
 if(!fd_valid(fd))return-LX_EBADF;
 LxFd *e=&fds[fd];
 if(e->kind==K_STDIN)return 0;
 if(e->kind==K_CONSOLE||e->kind==K_PIPE_W)return-LX_EBADF;
 if(e->kind==K_DIR)return-LX_EISDIR;
 if(e->kind==K_PIPE_R)return pipe_read((int)e->pipe,buffer,count);
 uint8_t *out=(uint8_t*)buffer;uint64_t done=0;
 while(done<count){
  uint64_t left=count-done;
  size_t want=left>SF_MSG_BYTES?SF_MSG_BYTES:(size_t)left;
  int64_t r=sf_file_read(&file_client,e->handle,out+done,want);
  if(r<0)return done?(int64_t)done:fail(r);
  if(!r)break;
  done+=(uint64_t)r;
 }
 return(int64_t)done;
}

static int64_t lx_write(int fd,const void *buffer,uint64_t count){
 if(!count)return 0;
 if(!buffer)return-LX_EFAULT;
 if(!fd_valid(fd))return-LX_EBADF;
 LxFd *e=&fds[fd];
 if(e->kind==K_STDIN||e->kind==K_PIPE_R)return-LX_EBADF;
 if(e->kind==K_PIPE_W)return pipe_write((int)e->pipe,buffer,count);
 uint64_t handle=e->handle;
 if(e->kind==K_CONSOLE){
  int64_t h=get_console();
  if(h<0)return fail(h);
  handle=(uint64_t)h;
 }
 const uint8_t *in=(const uint8_t*)buffer;uint64_t done=0;
 while(done<count){
  uint64_t left=count-done;
  size_t want=left>SF_MSG_BYTES?SF_MSG_BYTES:(size_t)left;
  int64_t r=sf_file_write(&file_client,handle,in+done,want);
  if(r<0)return done?(int64_t)done:fail(r);
  if(!r)break;
  done+=(uint64_t)r;
 }
 return(int64_t)done;
}

static int64_t lx_lseek(int fd,int64_t offset,int whence){
 if(!fd_valid(fd))return-LX_EBADF;
 LxFd *e=&fds[fd];
 if(e->kind!=K_FILE&&e->kind!=K_DIR)return-LX_ESPIPE;
 int64_t base=0;
 if(whence!=LX_SEEK_SET){
  SfFileStat st;
  int64_t r=sf_file_stat(&file_client,e->handle,&st);
  if(r<0)return fail(r);
  if(whence==LX_SEEK_CUR)base=(int64_t)st.offset;
  else if(whence==LX_SEEK_END)base=(int64_t)st.size;
  else return-LX_EINVAL;
 }
 if((offset>0&&base>INT64_MAX-offset)||(offset<0&&base<INT64_MIN-offset))return-LX_EINVAL;
 int64_t position=base+offset;
 if(position<0)return-LX_EINVAL;
 int64_t r=sf_file_seek(&file_client,e->handle,(uint64_t)position);
 if(r<0)return fail(r);
 if(r!=position)return-LX_EIO;
 return position;
}

static void fill_stat(int fd,LxStat *out){
 memset(out,0,sizeof(*out));
 out->st_dev=1;
 out->st_nlink=1;
 out->st_blksize=4096;
 LxFd *e=&fds[fd];
 switch(e->kind){
  case K_CONSOLE:out->st_mode=LX_S_IFCHR|0620;out->st_rdev=0x0501;break;
  case K_STDIN:out->st_mode=LX_S_IFCHR|0620;out->st_rdev=0x0500;break;
  case K_PIPE_R:case K_PIPE_W:out->st_mode=LX_S_IFIFO|0600;break;
  case K_FILE:case K_DIR:{
   SfFileStat st;
   if(sf_file_stat(&file_client,e->handle,&st)==0){
    out->st_size=st.size;
    out->st_blocks=(st.size+511)/512;
    out->st_ino=e->handle;
    out->st_mode=st.type==SF_TYPE_DIRECTORY?LX_S_IFDIR|0755:LX_S_IFREG|0644;
   }else{
    out->st_mode=LX_S_IFREG|0644;
   }
   break;
  }
  default:break;
 }
}

static int64_t lx_fstat(int fd,void *buffer){
 if(!fd_valid(fd))return-LX_EBADF;
 if(!buffer)return-LX_EFAULT;
 fill_stat(fd,(LxStat*)buffer);
 return 0;
}

static int64_t lx_newfstatat(int64_t dirfd,const char *path,void *buffer,int64_t flags){
 if(!buffer||!path)return-LX_EFAULT;
 if((flags&LX_AT_EMPTY_PATH)&&!*path)return lx_fstat((int)dirfd,buffer);
 int fd=(int)lx_openat(dirfd,path,LX_O_RDONLY,0);
 if(fd<0){
  if(fd!=-LX_ENOTDIR&&fd!=-LX_EISDIR)return fd;
  fd=(int)lx_openat(dirfd,path,LX_O_RDONLY|LX_O_DIRECTORY,0);
  if(fd<0)return fd;
 }
 int64_t r=lx_fstat(fd,buffer);
 fd_close(fd);
 return r;
}

static int64_t lx_getdents64(int fd,void *buffer,uint64_t count){
 if(!buffer)return-LX_EFAULT;
 if(!fd_valid(fd))return-LX_EBADF;
 LxFd *e=&fds[fd];
 if(e->kind!=K_DIR)return-LX_ENOTDIR;
 uint8_t *out=(uint8_t*)buffer;uint64_t used=0;
 for(;;){
  char name[256];
  int64_t r=sf_file_enumerate_at(&file_client,e->handle,e->index,name,sizeof(name));
  if(r<0)return used?(int64_t)used:fail(r);
  if(!r)break;
  size_t n=(size_t)r;
  if(n&&!name[n-1])n--;
  if(!n){e->index++;continue;}
  size_t record=(LX_DIRENT_NAME_OFF+n+1+7)&~7ull;
  if(used+record>count)break;
  LxDirent64 *d=(LxDirent64*)(out+used);
  memset(d,0,record);
  d->d_ino=e->index+1;
  d->d_off=(int64_t)e->index+1;
  d->d_reclen=(uint16_t)record;
  d->d_type=LX_DT_UNKNOWN;
  memcpy(out+used+LX_DIRENT_NAME_OFF,name,n);
  used+=record;
  e->index++;
 }
 return(int64_t)used;
}

/* ---- 内存类 ---- */
static int64_t lx_mmap(uint64_t address,uint64_t length,int prot,int flags,int fd,uint64_t offset){
 (void)address;(void)prot;
 if(!length)return-LX_EINVAL;
 if(flags&LX_MAP_FIXED)return-LX_ENOSYS;
 if(flags&~((int)LX_MAP_PRIVATE|(int)LX_MAP_ANONYMOUS))return-LX_EINVAL;
 if(!(flags&LX_MAP_ANONYMOUS))return-LX_ENODEV;
 if(fd>=0)return-LX_ENODEV;
 if(offset)return-LX_EINVAL;
 if(length>SF_HEAP_BYTES)return-LX_ENOMEM;
 uint64_t want=(length+SF_PAGE-1)&~(SF_PAGE-1);
 int64_t r=lx_sys(SYS_MMAP,0,want,0);
 return r<0?fail(r):r;
}

static int64_t lx_mprotect(uint64_t address,uint64_t length,int prot){
 if(!length)return-LX_EINVAL;
 if(address&(SF_PAGE-1))return-LX_EINVAL;
 if(prot&~(LX_PROT_READ|LX_PROT_WRITE|LX_PROT_EXEC))return-LX_EINVAL;
 if(prot&LX_PROT_EXEC)return-LX_ENOSYS;
 return 0;
}

static int64_t lx_brk(uint64_t address){
 if(!brk_cur){
  int64_t r=lx_sys(SYS_MMAP,0,LX_BRK_BYTES,0);
  if(r<0)return fail(r);
  brk_base=brk_cur=(uint64_t)r;
  brk_end=brk_base+LX_BRK_BYTES;
 }
 if(address&&address>=brk_base&&address<=brk_end)brk_cur=address;
 return(int64_t)brk_cur;
}

/* ---- 时间/进程/信息类 ---- */
static uint64_t current_ticks(void){
 int64_t t=lx_sys(SYS_TICKS,0,0,0);
 return t<0?0:(uint64_t)t;
}

static int64_t lx_clock_gettime(int clock,LxTimespec *out){
 if(!out)return-LX_EFAULT;
 if(clock!=0&&clock!=1)return-LX_EINVAL;
 uint64_t ticks=current_ticks();
 out->tv_sec=ticks/100;
 out->tv_nsec=(ticks%100)*10000000ull;
 return 0;
}

static int64_t lx_gettimeofday(LxTimeval *out,void *timezone){
 (void)timezone;
 if(!out)return-LX_EFAULT;
 uint64_t ticks=current_ticks();
 out->tv_sec=ticks/100;
 out->tv_usec=(ticks%100)*10000ull;
 return 0;
}

static int64_t lx_nanosleep(const LxTimespec *request,LxTimespec *remaining){
 if(!request)return-LX_EFAULT;
 if(request->tv_sec<0||request->tv_nsec<0||request->tv_nsec>=1000000000)return-LX_EINVAL;
 uint64_t ms=(uint64_t)request->tv_sec*1000+(uint64_t)request->tv_nsec/1000000;
 if(ms>10000)ms=10000;
 uint64_t until=current_ticks()+(ms+9)/10;
 while(current_ticks()<until)lx_sys(SYS_YIELD,0,0,0);
 if(remaining){remaining->tv_sec=0;remaining->tv_nsec=0;}
 return 0;
}

static void copy_field(char *dst,size_t size,const char *src){
 size_t n=0;
 while(n+1<size&&src[n]){dst[n]=src[n];n++;}
 dst[n]=0;
}

static int64_t lx_uname(LxUtsname *out){
 if(!out)return-LX_EFAULT;
 memset(out,0,sizeof(*out));
 copy_field(out->sysname,sizeof(out->sysname),"Linux");
 copy_field(out->nodename,sizeof(out->nodename),"spiritfox");
 copy_field(out->release,sizeof(out->release),"6.1.0-spiritfox");
 copy_field(out->version,sizeof(out->version),"S3");
 copy_field(out->machine,sizeof(out->machine),"x86_64");
 copy_field(out->domainname,sizeof(out->domainname),"localdomain");
 return 0;
}

static int64_t lx_sysinfo(LxSysinfo *out){
 if(!out)return-LX_EFAULT;
 memset(out,0,sizeof(*out));
 out->uptime=(int64_t)(current_ticks()/100);
 out->totalram=256ull*1024*1024;
 out->freeram=192ull*1024*1024;
 out->totalswap=0;
 out->freeswap=0;
 out->procs=1;
 out->mem_unit=1;
 return 0;
}

static int64_t lx_prlimit64(int64_t pid,int resource,const LxRlimit *new_limit,LxRlimit *old_limit){
 if(pid!=0&&(!boot_args||pid!=(int64_t)boot_args->pid))return-LX_ESRCH;
 if(new_limit)return-LX_EPERM;
 if(resource<LX_RLIMIT_CPU||resource>LX_RLIMIT_RTTIME)return-LX_EINVAL;
 if(old_limit){
  uint64_t value=0;
  switch(resource){
   case LX_RLIMIT_NOFILE:value=64;break;
   case LX_RLIMIT_STACK:value=8ull*1024*1024;break;
   case LX_RLIMIT_DATA:case LX_RLIMIT_AS:value=256ull*1024*1024;break;
   case LX_RLIMIT_NPROC:value=32;break;
   default:value=0;break;
  }
  old_limit->cur=value;
  old_limit->max=value;
 }
 return 0;
}

/* ---- 同步/描述符类 ---- */
static int64_t lx_poll(LxPollfd *list,uint64_t count,int64_t timeout){
 (void)timeout;
 if(!count){lx_sys(SYS_YIELD,0,0,0);return 0;}
 if(!list)return-LX_EFAULT;
 int64_t ready=0;
 for(uint64_t i=0;i<count;i++){
  int fd=list[i].fd;
  list[i].revents=0;
  if(fd<0)continue;
  if(!fd_valid(fd)){list[i].revents=LX_POLLNVAL;ready++;continue;}
  short events=0;
  switch(fds[fd].kind){
   case K_STDIN:case K_FILE:case K_DIR:case K_PIPE_R:events|=LX_POLLIN;break;
   default:break;
  }
  if(fds[fd].kind==K_CONSOLE||fds[fd].kind==K_FILE||fds[fd].kind==K_PIPE_W)events|=LX_POLLOUT;
  list[i].revents=events;
  if(events)ready++;
 }
 return ready;
}

static int64_t lx_ioctl(int fd,uint64_t request,void *argument){
 if(!fd_valid(fd))return-LX_EBADF;
 if(request==LX_TIOCGWINSZ){
  if(!argument)return-LX_EFAULT;
  LxWinsize *ws=(LxWinsize*)argument;
  ws->ws_row=25;ws->ws_col=80;ws->ws_xpixel=0;ws->ws_ypixel=0;
  return 0;
 }
 return-LX_ENOTTY;
}

static int64_t lx_pipe2(int *out,int flags){
 if(!out)return-LX_EFAULT;
 if(flags&~(LX_O_CLOEXEC|LX_O_NONBLOCK))return-LX_EINVAL;
 int slot=pipe_alloc();
 if(slot<0)return slot;
 int64_t raw=lx_sys(SYS_MMAP,0,LX_PIPE_BYTES,0);
 if(raw<0){pipes[slot]=(LxPipe){0,0,0,0,0,0,0};return fail(raw);}
 int read_fd=fd_alloc();
 int write_fd=-1;
 if(read_fd>=0){
  /* 先占住读端槽位，否则第二次 fd_alloc 会返回同一个槽位。 */
  fds[read_fd].kind=K_PIPE_R;
  write_fd=fd_alloc();
 }
 if(read_fd<0||write_fd<0){
  int error=read_fd<0?read_fd:write_fd;
  if(read_fd>=0)fds[read_fd].kind=K_NONE;
  lx_sys(SYS_MUNMAP,(uint64_t)raw,LX_PIPE_BYTES,0);
  pipes[slot]=(LxPipe){0,0,0,0,0,0,0};
  return error;
 }
 LxPipe *p=&pipes[slot];
 p->used=1;p->readers=1;p->writers=1;p->buffer=(uint8_t*)(uintptr_t)raw;
 fds[read_fd].kind=K_PIPE_R;fds[read_fd].pipe=(uint32_t)slot;
 fds[write_fd].kind=K_PIPE_W;fds[write_fd].pipe=(uint32_t)slot;
 out[0]=read_fd;out[1]=write_fd;
 return 0;
}

static int64_t lx_dup(int fd){
 if(!fd_valid(fd))return-LX_EBADF;
 int target=fd_alloc();
 if(target<0)return target;
 int64_t r=fd_clone(fd,target);
 if(r<0){fds[target].kind=K_NONE;return r;}
 return target;
}

static int64_t lx_dup2(int fd,int target){
 if(!fd_valid(fd))return-LX_EBADF;
 return fd_clone(fd,target);
}

static int64_t lx_dup3(int fd,int target,int flags){
 if(flags&~LX_O_CLOEXEC)return-LX_EINVAL;
 if(fd==target)return-LX_EINVAL;
 return lx_dup2(fd,target);
}

static int64_t lx_fcntl(int fd,int command,int64_t argument){
 if(!fd_valid(fd))return-LX_EBADF;
 (void)argument;
 switch(command){
  case LX_F_DUPFD:case LX_F_DUPFD_CLOEXEC:return lx_dup(fd);
  case LX_F_GETFD:case LX_F_SETFD:case LX_F_SETFL:return 0;
  case LX_F_GETFL:return 0;
  default:return-LX_EINVAL;
 }
}

static int64_t lx_getcwd(char *buffer,uint64_t size){
 if(!buffer)return-LX_EFAULT;
 if(size<2)return-LX_ERANGE;
 buffer[0]='/';buffer[1]=0;
 return 2;
}

/* ---- 拒绝策略 ---- */
static int dangerous(int64_t number){
 switch(number){
  case LX_SYS_PTRACE:case LX_SYS_SETUID:case LX_SYS_SETGID:
  case LX_SYS_SETRESUID:case LX_SYS_SETRESGID:
  case LX_SYS_MOUNT:case LX_SYS_UMOUNT2:case LX_SYS_SWAPON:
  case LX_SYS_SWAPOFF:case LX_SYS_REBOOT:case LX_SYS_INIT_MODULE:
  case LX_SYS_FINIT_MODULE:case LX_SYS_DELETE_MODULE:case LX_SYS_KEXEC_LOAD:
  case LX_SYS_PROCESS_VM_READV:case LX_SYS_PROCESS_VM_WRITEV:
  case LX_SYS_BPF:case LX_SYS_USERFAULTFD:
  case LX_SYS_IO_URING_SETUP:case LX_SYS_IO_URING_ENTER:case LX_SYS_IO_URING_REGISTER:
   return 1;
  default:return 0;
 }
}

static int64_t do_exit(int64_t status){
 lx_sys(SYS_EXIT,(uint64_t)(status&0xff),0,0);
 for(;;)lx_sys(SYS_YIELD,0,0,0);
}

int64_t sf_linux_dispatch(LxFrame *frame){
 if(!frame)return-LX_EFAULT;
 int64_t number=(int64_t)frame->rax;
 int64_t a=(int64_t)frame->rdi,b=(int64_t)frame->rsi,c=(int64_t)frame->rdx;
 int64_t d=(int64_t)frame->r10,e=(int64_t)frame->r8,g=(int64_t)frame->r9;
 int64_t result;
 if(number==LX_SYS_EXIT||number==LX_SYS_EXIT_GROUP)return do_exit(a);
 int status=io_init();
 if(status)return status;
 if(dangerous(number)){result=-LX_EPERM;goto out;}
 switch(number){
  case LX_SYS_READ:result=lx_read((int)a,(void*)(uintptr_t)b,(uint64_t)c);break;
  case LX_SYS_WRITE:result=lx_write((int)a,(const void*)(uintptr_t)b,(uint64_t)c);break;
  case LX_SYS_OPEN:result=lx_openat(LX_AT_FDCWD,(const char*)(uintptr_t)a,b,c);break;
  case LX_SYS_OPENAT:result=lx_openat(a,(const char*)(uintptr_t)b,c,d);break;
  case LX_SYS_CLOSE:result=fd_close((int)a);break;
  case LX_SYS_FSTAT:result=lx_fstat((int)a,(void*)(uintptr_t)b);break;
  case LX_SYS_POLL:result=lx_poll((LxPollfd*)(uintptr_t)a,(uint64_t)b,c);break;
  case LX_SYS_LSEEK:result=lx_lseek((int)a,b,(int)c);break;
  case LX_SYS_MMAP:result=lx_mmap((uint64_t)a,(uint64_t)b,(int)c,(int)d,(int)e,g);break;
  case LX_SYS_MPROTECT:result=lx_mprotect((uint64_t)a,(uint64_t)b,(int)c);break;
  case LX_SYS_MUNMAP:{
   int64_t r=lx_sys(SYS_MUNMAP,(uint64_t)a,(uint64_t)b,0);
   result=r<0?fail(r):0;
   break;
  }
  case LX_SYS_BRK:result=lx_brk((uint64_t)a);break;
  case LX_SYS_IOCTL:result=lx_ioctl((int)a,(uint64_t)b,(void*)(uintptr_t)c);break;
  case LX_SYS_SCHED_YIELD:{
   int64_t r=lx_sys(SYS_YIELD,0,0,0);
   result=r<0?fail(r):0;
   break;
  }
  case LX_SYS_DUP:result=lx_dup((int)a);break;
  case LX_SYS_DUP2:result=lx_dup2((int)a,(int)b);break;
  case LX_SYS_DUP3:result=lx_dup3((int)a,(int)b,(int)c);break;
  case LX_SYS_NANOSLEEP:result=lx_nanosleep((const LxTimespec*)(uintptr_t)a,(LxTimespec*)(uintptr_t)b);break;
  case LX_SYS_GETPID:case LX_SYS_GETTID:result=boot_args?(int64_t)boot_args->pid:-LX_EIO;break;
  case LX_SYS_GETUID:result=0;break;
  case LX_SYS_UNAME:result=lx_uname((LxUtsname*)(uintptr_t)a);break;
  case LX_SYS_FCNTL:result=lx_fcntl((int)a,(int)b,c);break;
  case LX_SYS_GETCWD:result=lx_getcwd((char*)(uintptr_t)a,(uint64_t)b);break;
  case LX_SYS_GETTIMEOFDAY:result=lx_gettimeofday((LxTimeval*)(uintptr_t)a,(void*)(uintptr_t)b);break;
  case LX_SYS_SYSINFO:result=lx_sysinfo((LxSysinfo*)(uintptr_t)a);break;
  case LX_SYS_GETDENTS64:result=lx_getdents64((int)a,(void*)(uintptr_t)b,(uint64_t)c);break;
  case LX_SYS_SET_TID_ADDRESS:tid_address=(uint64_t)a;result=boot_args?(int64_t)boot_args->pid:-LX_EIO;break;
  case LX_SYS_CLOCK_GETTIME:result=lx_clock_gettime((int)a,(LxTimespec*)(uintptr_t)b);break;
  case LX_SYS_NEWFSTATAT:result=lx_newfstatat(a,(const char*)(uintptr_t)b,(void*)(uintptr_t)c,d);break;
  case LX_SYS_PPOLL:result=lx_poll((LxPollfd*)(uintptr_t)a,(uint64_t)b,c);break;
  case LX_SYS_PIPE2:result=lx_pipe2((int*)(uintptr_t)a,(int)b);break;
  case LX_SYS_PRLIMIT64:result=lx_prlimit64(a,(int)b,(const LxRlimit*)(uintptr_t)c,(LxRlimit*)(uintptr_t)d);break;
  case LX_SYS_ARCH_PRCTL:
   result=(a==LX_ARCH_SET_FS||a==LX_ARCH_GET_FS)?-LX_ENOSYS:-LX_EINVAL;
   break;
  case LX_SYS_GETRANDOM:result=-LX_ENOSYS;break;
  case LX_SYS_READLINKAT:result=-LX_EINVAL;break;
  case LX_SYS_EXIT:case LX_SYS_EXIT_GROUP:result=do_exit(a);break;
  default:result=-LX_ENOSYS;break;
 }
out:
 frame->rax=(uint64_t)result;
 return result;
}
