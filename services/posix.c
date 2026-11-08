/* Bounded POSIX subset runtime library (image: sf-libsf.sfmod).
 *
 * The image is loaded into the application address space by sf-init and
 * exports the functions declared in include/posix.h. Applications bind them
 * through the package import table (ABI libsf-posix-1).
 *
 * Capability model: mutations that would require a write capability absent
 * from ordinary applications report EACCES instead of silently escalating. */
#include "posix.h"
#include "libsf.h"
#include "sf_file.h"
#include "arguments.h"
#include "sdk.h"
#include <stdarg.h>

static SfFileClient client;
static const SfBootArgs *arguments;
static int initialized;
static int console=-1;
static uint64_t root;
static int root_valid;
static int error_value;

int *__errno_location(void){return &error_value;}
static int fail(int64_t status){
 switch(status){
  case SF_NOT_FOUND:error_value=ENOENT;break;
  case SF_DENIED:error_value=EACCES;break;
  case SF_NO_SPACE:error_value=ENOSPC;break;
  case SF_STALE_HANDLE:error_value=EBADF;break;
  case SF_BAD_FORMAT:case SF_INVALID:error_value=EINVAL;break;
  default:error_value=EIO;
 }
 return -1;
}
static long long_fail(int64_t status){fail(status);return -1;}
static int setup(void){
 if(initialized)return 0;
 const SfBootArgs *a=(const SfBootArgs*)(uintptr_t)SF_ARGS;
 if(!a||a->abi_major!=SF_ABI||sf_boot_arguments_validate(a,a->size)){error_value=EIO;return -1;}
 if(sf_file_client_init(&client,a->manager,a->service,3000)){error_value=EIO;return -1;}
 arguments=a;initialized=1;return 0;
}
static int root_handle(void){
 if(root_valid)return 0;
 int64_t handle=sf_file_root(&client);
 if(handle<0)return fail(handle);
 root=(uint64_t)handle;root_valid=1;return 0;
}
static int console_handle(void){
 if(console>=0)return console;
 int64_t handle=sf_file_console(&client);
 if(handle<0)return fail(handle);
 console=(int)handle;return console;
}
static const char *relative_path(const char *path){
 if(!path)return 0;
 while(*path=='/')path++;
 return *path?path:".";
}

int open(const char *path,int flags,...){
 if(setup())return -1;
 const char *name=relative_path(path);
 if(!name){error_value=EINVAL;return -1;}
 if(flags&(O_CREAT|O_TRUNC)){error_value=EACCES;return -1;}
 if(!strcmp(name,"dev/console")&&(flags&O_ACCMODE)!=O_RDONLY){
  if(flags&O_DIRECTORY){error_value=ENOTDIR;return -1;}
  int64_t handle=sf_file_console(&client);
  if(handle<0)return fail(handle);
  return(int)handle;
 }
 uint32_t mode=(flags&O_ACCMODE)==O_RDONLY?SF_OPEN_READ:SF_OPEN_WRITE;
 if(flags&O_DIRECTORY)mode|=SF_OPEN_DIRECTORY;
 if(root_handle())return -1;
 int64_t handle=sf_file_openat(&client,root,name,mode);
 if(handle<0)return fail(handle);
 return(int)handle;
}
long read(int fd,void *buffer,unsigned long bytes){
 if(fd<=0||(!buffer&&bytes)){error_value=EBADF;return -1;}
 int64_t result=sf_file_read(&client,(uint64_t)fd,buffer,bytes);
 return result<0?long_fail(result):(long)result;
}
long write(int fd,const void *buffer,unsigned long bytes){
 if(fd<=0||(!buffer&&bytes)){error_value=EBADF;return -1;}
 unsigned long done=0;
 while(done<bytes){
  int64_t result=sf_file_write(&client,(uint64_t)fd,(const uint8_t*)buffer+done,bytes-done);
  if(result<0){if(done)return(long)done;return long_fail(result);}
  if(!result)break;
  done+=(unsigned long)result;
 }
 return(long)done;
}
int close(int fd){
 if(fd<=0){error_value=EBADF;return -1;}
 int64_t result=sf_file_close(&client,(uint64_t)fd);
 return result<0?fail(result):0;
}
long lseek(int fd,long offset,int whence){
 int64_t base=0;
 if(whence!=SEEK_SET){
  struct stat info;
  if(fstat(fd,&info))return -1;
  if(whence==SEEK_CUR)base=(int64_t)info.offset;else if(whence==SEEK_END)base=(int64_t)info.size;
  else{error_value=EINVAL;return -1;}
 }
 if((offset>0&&base>INT64_MAX-offset)||(offset<0&&base<INT64_MIN-offset)){error_value=EINVAL;return -1;}
 int64_t target=base+offset;
 if(target<0){error_value=EINVAL;return -1;}
 int64_t result=sf_file_seek(&client,(uint64_t)fd,(uint64_t)target);
 return result<0?long_fail(result):(long)result;
}
int fstat(int fd,struct stat *info){
 if(fd<=0||!info){error_value=EBADF;return -1;}
 SfFileStat value;
 int64_t result=sf_file_stat(&client,(uint64_t)fd,&value);
 if(result<0)return fail(result);
 info->type=value.type;info->mode=value.mode;info->flags=value.flags;
 info->size=value.size;info->offset=value.offset;
 return 0;
}
int stat(const char *path,struct stat *info){
 if(setup())return -1;
 const char *name=relative_path(path);
 if(!name){error_value=EINVAL;return -1;}
 if(root_handle())return -1;
 int64_t handle=sf_file_openat(&client,root,name,SF_OPEN_READ);
 if(handle<0)handle=sf_file_openat(&client,root,name,SF_OPEN_READ|SF_OPEN_DIRECTORY);
 if(handle<0)return fail(handle);
 int result=fstat((int)handle,info);
 int64_t closed=sf_file_close(&client,(uint64_t)handle);
 if(!result&&closed<0)result=fail(closed);
 return result;
}
int getpid(void){
 if(setup())return -1;
 return(int)arguments->pid;
}
void _exit(int status){
 sf_sys(SYS_EXIT,(uint64_t)status,0,0);
 for(;;)sf_yield();
}
unsigned sleep(unsigned seconds){
 if(setup()||seconds>60)return seconds;
 uint64_t want=(uint64_t)seconds*100ull,start=sf_ticks();
 while(sf_ticks()-start<want)sf_yield();
 return 0;
}
int printf(const char *format,...){
 if(setup())return -1;
 char buffer[512];va_list args;va_start(args,format);
 int length=sf_vsnprintf(buffer,sizeof(buffer),format,args);
 va_end(args);
 int handle=console_handle();
 if(handle<0)return -1;
 unsigned long bytes=(unsigned long)length;
 if(bytes>=sizeof(buffer))bytes=sizeof(buffer)-1;
 long written=write(handle,buffer,bytes);
 return written<0?-1:(int)written;
}
int puts(const char *text){
 if(!text){error_value=EINVAL;return -1;}
 if(printf("%s\n",text)<=0)return -1;
 return 0;
}
char *getenv(const char *name){
 if(setup()||!name)return 0;
 return(char*)sf_environment_value(arguments,arguments->size,name);
}
const char *strerror(int value){
 switch(value){
  case 0:return "success";
  case ENOENT:return "no such file";
  case EIO:return "i/o error";
  case EBADF:return "bad file descriptor";
  case EACCES:return "permission denied";
  case ENOTDIR:return "not a directory";
  case EINVAL:return "invalid argument";
  case ENOSPC:return "no space left";
  case ENOSYS:return "not implemented";
  default:return "unknown error";
 }
}
/* The runtime image is never spawned as a process; this entry exists only so
 * the relative-v1 ELF has a valid entry point. */
long sf_main(const SfBootArgs *a,size_t bytes){(void)a;(void)bytes;return 0;}
