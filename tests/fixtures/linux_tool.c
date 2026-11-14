/* S3 工具夹具：同一份源码按 -DTOOL_* 生成 7 个静态 PIE。
 * 全部行为固定（不依赖 argv），输出同时作为 QEMU 断言依据。 */
#include "linux_raw.h"

static int tool_main(void){
#if defined(TOOL_TRUE)
 lx_puts("LINUX TOOL TRUE OK\n");
 return 0;
#elif defined(TOOL_FALSE)
 lx_puts("LINUX TOOL FALSE OK\n");
 return 1;
#elif defined(TOOL_ECHO)
 lx_puts("LINUX TOOL ECHO OK\n");
 return 0;
#elif defined(TOOL_SEQ)
 for(int i=1;i<=5;i++){char line[2];line[0]=(char)('0'+i);line[1]='\n';lx_write(1,line,2);}
 lx_puts("LINUX TOOL SEQ OK\n");
 return 0;
#elif defined(TOOL_CAT)
 long fd=lx_raw(LX_SYS_OPENAT,LX_AT_FDCWD,(long)"etc/readme.txt",LX_O_RDONLY,0,0,0);
 if(fd<0){lx_puts("LINUX TOOL CAT FAIL\n");return 1;}
 char buffer[128];
 for(;;){
  long r=lx_raw(LX_SYS_READ,fd,(long)buffer,sizeof(buffer),0,0,0);
  if(r<0){lx_raw(LX_SYS_CLOSE,fd,0,0,0,0,0);lx_puts("LINUX TOOL CAT FAIL\n");return 1;}
  if(!r)break;
  if(lx_write(1,buffer,(unsigned long)r)!=r){lx_raw(LX_SYS_CLOSE,fd,0,0,0,0,0);return 1;}
 }
 lx_raw(LX_SYS_CLOSE,fd,0,0,0,0,0);
 lx_puts("LINUX TOOL CAT OK\n");
 return 0;
#elif defined(TOOL_WC)
 long fd=lx_raw(LX_SYS_OPENAT,LX_AT_FDCWD,(long)"etc/readme.txt",LX_O_RDONLY,0,0,0);
 if(fd<0){lx_puts("LINUX TOOL WC FAIL\n");return 1;}
 char buffer[128];
 unsigned long bytes=0,words=0,lines=0;
 int in_word=0;
 for(;;){
  long r=lx_raw(LX_SYS_READ,fd,(long)buffer,sizeof(buffer),0,0,0);
  if(r<0){lx_raw(LX_SYS_CLOSE,fd,0,0,0,0,0);lx_puts("LINUX TOOL WC FAIL\n");return 1;}
  if(!r)break;
  bytes+=(unsigned long)r;
  for(long i=0;i<r;i++){
   char ch=buffer[i];
   if(ch=='\n')lines++;
   int space=ch==' '||ch=='\t'||ch=='\n'||ch=='\r';
   if(space)in_word=0;
   else if(!in_word){in_word=1;words++;}
  }
 }
 lx_raw(LX_SYS_CLOSE,fd,0,0,0,0,0);
 lx_puts("WC ");
 lx_putnum((long)bytes);lx_puts(" ");
 lx_putnum((long)words);lx_puts(" ");
 lx_putnum((long)lines);lx_puts("\n");
 lx_puts("LINUX TOOL WC OK\n");
 return 0;
#elif defined(TOOL_LS)
 long fd=lx_raw(LX_SYS_OPENAT,LX_AT_FDCWD,(long)"etc",LX_O_RDONLY|LX_O_DIRECTORY,0,0,0);
 if(fd<0){lx_puts("LINUX TOOL LS FAIL\n");return 1;}
 char buffer[512];
 for(;;){
  long r=lx_raw(LX_SYS_GETDENTS64,fd,(long)buffer,sizeof(buffer),0,0,0);
  if(r<0){lx_raw(LX_SYS_CLOSE,fd,0,0,0,0,0);lx_puts("LINUX TOOL LS FAIL\n");return 1;}
  if(!r)break;
  long offset=0;
  while(offset<r){
   LxDirent64 *entry=(LxDirent64*)(buffer+offset);
   if(!entry->d_reclen)break;
   const char *name=buffer+offset+LX_DIRENT_NAME_OFF;
   if(name[0]){lx_puts("LS ");lx_puts(name);lx_puts("\n");}
   offset+=entry->d_reclen;
  }
 }
 lx_raw(LX_SYS_CLOSE,fd,0,0,0,0,0);
 lx_puts("LINUX TOOL LS OK\n");
 return 0;
#else
#error "define one TOOL_*"
#endif
}

long sf_tool_main(long argc,char **argv){
 (void)argc;(void)argv;
 return tool_main();
}
