#ifndef SF_POSIX_H
#define SF_POSIX_H
#include "sf.h"
/* Bounded POSIX compatibility subset for native applications.
 *
 * This layer runs inside the application address space as the authenticated
 * runtime image sf-libsf.sfmod and is imported by name via the package import
 * table (ABI "libsf-posix-1"). It maps calls onto the existing capability
 * model: read-only applications can open/read/stat files they are delegated,
 * while write/mutation operations return EACCES when the capability is absent.
 *
 * It is intentionally not a complete POSIX implementation: there is no libc,
 * no malloc, no signals, no threads, no fork/exec, no pipes and no dynamic
 * data segments. Only the functions listed here are part of the ABI. */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_ACCMODE 3
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_DIRECTORY 0200000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define ENOENT 2
#define EIO 5
#define EBADF 9
#define EACCES 13
#define ENOTDIR 20
#define EINVAL 22
#define ENOSPC 28
#define ENOSYS 38

struct stat {
 uint32_t type,mode,flags;
 uint64_t size,offset;
};

int *__errno_location(void);
#define errno (*__errno_location())
const char *strerror(int);

int open(const char *,int,...);
long read(int,void *,unsigned long);
long write(int,const void *,unsigned long);
int close(int);
long lseek(int,long,int);
int fstat(int,struct stat *);
int stat(const char *,struct stat *);
int getpid(void);
void _exit(int);
unsigned sleep(unsigned);
int printf(const char *, ...);
int puts(const char *);
char *getenv(const char *);
#endif
