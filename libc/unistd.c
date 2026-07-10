/* SpiritFoxOS libc - POSIX unistd wrappers */

#include "arch/x86_64/syscall.h"
#include <sys/types.h>
#include <stddef.h>

#define SYS_read       0
#define SYS_write      1
#define SYS_close      3
#define SYS_lseek      8
#define SYS_getpid    39
#define SYS_fork      57
#define SYS_execve    59
#define SYS_exit      60
#define SYS_getuid   102
#define SYS_getgid   104
#define SYS_setuid   105
#define SYS_setgid   106
#define SYS_geteuid  107
#define SYS_getegid  108

ssize_t read(int fd, void *buf, size_t count)
{
    return (ssize_t)sfk_syscall3(SYS_read, fd, (int64_t)buf, count);
}

ssize_t write(int fd, const void *buf, size_t count)
{
    return (ssize_t)sfk_syscall3(SYS_write, fd, (int64_t)buf, count);
}

int close(int fd)
{
    return (int)sfk_syscall1(SYS_close, fd);
}
