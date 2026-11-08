/* Probe for the bounded POSIX runtime library import table. */
#include "sf.h"
#include "sf_file.h"
#include "arguments.h"
#include "posix_imports.h"

int (*__sf_import_open)(const char *,int,...);
long (*__sf_import_read)(int,void *,unsigned long);
long (*__sf_import_write)(int,const void *,unsigned long);
int (*__sf_import_close)(int);
long (*__sf_import_lseek)(int,long,int);
int (*__sf_import_fstat)(int,struct stat *);
int (*__sf_import_stat)(const char *,struct stat *);
int (*__sf_import_getpid)(void);
void (*__sf_import__exit)(int);
unsigned (*__sf_import_sleep)(unsigned);
int (*__sf_import_printf)(const char *, ...);
int (*__sf_import_puts)(const char *);
char *(*__sf_import_getenv)(const char *);
int *(*__sf_import___errno_location)(void);
const char *(*__sf_import_strerror)(int);

long sf_main(const SfBootArgs *a,size_t bytes){
 if(sf_boot_arguments_validate(a,bytes))return 1;
 int pid=getpid();
 if(pid<=0)return 2;
 int fd=open("/dev/console",O_WRONLY);
 if(fd<0)return 3;
 struct stat info;
 if(fstat(fd,&info)||info.type!=SF_TYPE_CONSOLE){close(fd);return 4;}
 if(write(fd,"POSIX ",6)<0){close(fd);return 5;}
 close(fd);
 int notes=open("notes",O_RDONLY);
 if(notes<0)return 6;
 char buffer[64];
 long got=read(notes,buffer,32);
 if(got<0){close(notes);return 7;}
 close(notes);
 if(open("missing",O_RDONLY)!=-1)return 8;
 if(errno!=ENOENT)return 9;
 if(printf("PASS pid=%d libc=%s\n",pid,strerror(0))<0)return 10;
 if(puts("POSIX imports bound by name/ABI")<0)return 11;
 sleep(0);
 return 0;
}
