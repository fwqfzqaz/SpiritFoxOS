/* SpiritFoxOS libc - Syscall wrappers
 * Implements POSIX syscall wrappers using the Linux-compatible
 * syscall ABI provided by the SpiritFoxOS kernel.
 */

#include "arch/x86_64/syscall.h"
#include <sys/types.h>
#include <stddef.h>
#include <stdarg.h>
#include <errno.h>
#include <mman.h>
#include <time.h>

/* Syscall numbers (Linux x86_64 ABI) */
#define SYS_read       0
#define SYS_write      1
#define SYS_open       2
#define SYS_close      3
#define SYS_stat       4
#define SYS_fstat      5
#define SYS_lstat      6
#define SYS_poll       7
#define SYS_lseek      8
#define SYS_mmap       9
#define SYS_mprotect   10
#define SYS_munmap     11
#define SYS_brk        12
#define SYS_rt_sigaction  13
#define SYS_rt_sigprocmask 14
#define SYS_ioctl      16
#define SYS_pread64    17
#define SYS_pwrite64   18
#define SYS_access     21
#define SYS_pipe       22
#define SYS_select     23
#define SYS_sched_yield 24
#define SYS_dup        32
#define SYS_dup2       33
#define SYS_nanosleep  35
#define SYS_getpid     39
#define SYS_socket     41
#define SYS_connect    42
#define SYS_accept     43
#define SYS_sendto     44
#define SYS_recvfrom   45
#define SYS_clone      56
#define SYS_fork       57
#define SYS_execve     59
#define SYS_exit       60
#define SYS_wait4      61
#define SYS_kill       62
#define SYS_uname      63
#define SYS_fcntl      72
#define SYS_fsync      74
#define SYS_getcwd     79
#define SYS_chdir      80
#define SYS_rename     82
#define SYS_mkdir      83
#define SYS_rmdir      84
#define SYS_creat      85
#define SYS_unlink     87
#define SYS_readlink   89
#define SYS_chmod      90
#define SYS_chown      92
#define SYS_gettimeofday 96
#define SYS_getuid     102
#define SYS_getgid     104
#define SYS_setuid     105
#define SYS_setgid     106
#define SYS_geteuid    107
#define SYS_getegid    108
#define SYS_getppid    110
#define SYS_sigaltstack 131
#define SYS_arch_prctl 158
#define SYS_set_tid_address 218
#define SYS_exit_group 231
#define SYS_futex      202
#define SYS_clock_gettime 228
#define SYS_getrandom  318

/* Errno handling */
static int sfk_errno = 0;
int *__errno_location(void) { return &sfk_errno; }

#define SET_ERRNO(ret) do { \
    if ((ret) < 0) { sfk_errno = -(int)(ret); ret = -1; } \
    else { sfk_errno = 0; } \
} while(0)

/* File I/O */
ssize_t read(int fd, void *buf, size_t count)
{
    int64_t ret = sfk_syscall3(SYS_read, fd, (int64_t)buf, count);
    SET_ERRNO(ret);
    return (ssize_t)ret;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    int64_t ret = sfk_syscall3(SYS_write, fd, (int64_t)buf, count);
    SET_ERRNO(ret);
    return (ssize_t)ret;
}

int open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & 0x40) {  /* O_CREAT */
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    int64_t ret = sfk_syscall3(SYS_open, (int64_t)path, flags, mode);
    SET_ERRNO(ret);
    return (int)ret;
}

int close(int fd)
{
    int64_t ret = sfk_syscall1(SYS_close, fd);
    SET_ERRNO(ret);
    return (int)ret;
}

off_t lseek(int fd, off_t offset, int whence)
{
    int64_t ret = sfk_syscall3(SYS_lseek, fd, offset, whence);
    SET_ERRNO(ret);
    return (off_t)ret;
}

int dup(int fd)
{
    int64_t ret = sfk_syscall1(SYS_dup, fd);
    SET_ERRNO(ret);
    return (int)ret;
}

int dup2(int oldfd, int newfd)
{
    int64_t ret = sfk_syscall2(SYS_dup2, oldfd, newfd);
    SET_ERRNO(ret);
    return (int)ret;
}

int pipe(int pipefd[2])
{
    int64_t ret = sfk_syscall1(SYS_pipe, (int64_t)pipefd);
    SET_ERRNO(ret);
    return (int)ret;
}

int fcntl(int fd, int cmd, ...)
{
    int64_t arg = 0;
    va_list ap;
    va_start(ap, cmd);
    arg = va_arg(ap, int64_t);
    va_end(ap);
    int64_t ret = sfk_syscall3(SYS_fcntl, fd, cmd, arg);
    SET_ERRNO(ret);
    return (int)ret;
}

/* Directory */
int mkdir(const char *path, mode_t mode)
{
    int64_t ret = sfk_syscall2(SYS_mkdir, (int64_t)path, mode);
    SET_ERRNO(ret);
    return (int)ret;
}

int rmdir(const char *path)
{
    int64_t ret = sfk_syscall1(SYS_rmdir, (int64_t)path);
    SET_ERRNO(ret);
    return (int)ret;
}

int chdir(const char *path)
{
    int64_t ret = sfk_syscall1(SYS_chdir, (int64_t)path);
    SET_ERRNO(ret);
    return (int)ret;
}

char *getcwd(char *buf, size_t size)
{
    int64_t ret = sfk_syscall2(SYS_getcwd, (int64_t)buf, size);
    if (ret < 0) { sfk_errno = -(int)ret; return NULL; }
    return buf;
}

int unlink(const char *path)
{
    int64_t ret = sfk_syscall1(SYS_unlink, (int64_t)path);
    SET_ERRNO(ret);
    return (int)ret;
}

int rename(const char *oldpath, const char *newpath)
{
    int64_t ret = sfk_syscall2(SYS_rename, (int64_t)oldpath, (int64_t)newpath);
    SET_ERRNO(ret);
    return (int)ret;
}

int chmod(const char *path, mode_t mode)
{
    int64_t ret = sfk_syscall2(SYS_chmod, (int64_t)path, mode);
    SET_ERRNO(ret);
    return (int)ret;
}

/* Process */
pid_t getpid(void)
{
    return (pid_t)sfk_syscall0(SYS_getpid);
}

pid_t getppid(void)
{
    return (pid_t)sfk_syscall0(SYS_getppid);
}

uid_t getuid(void) { return (uid_t)sfk_syscall0(SYS_getuid); }
uid_t geteuid(void) { return (uid_t)sfk_syscall0(SYS_geteuid); }
gid_t getgid(void) { return (gid_t)sfk_syscall0(SYS_getgid); }
gid_t getegid(void) { return (gid_t)sfk_syscall0(SYS_getegid); }

int setuid(uid_t uid)
{
    int64_t ret = sfk_syscall1(SYS_setuid, uid);
    SET_ERRNO(ret);
    return (int)ret;
}

int setgid(gid_t gid)
{
    int64_t ret = sfk_syscall1(SYS_setgid, gid);
    SET_ERRNO(ret);
    return (int)ret;
}

pid_t fork(void)
{
    return (pid_t)sfk_syscall0(SYS_fork);
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    int64_t ret = sfk_syscall3(SYS_execve, (int64_t)path, (int64_t)argv, (int64_t)envp);
    SET_ERRNO(ret);
    return (int)ret;
}

void _exit(int status)
{
    sfk_syscall1(SYS_exit, status);
    __builtin_unreachable();
}

void exit(int status)
{
    sfk_syscall1(SYS_exit_group, status);
    __builtin_unreachable();
}

pid_t wait4(pid_t pid, int *status, int options, void *rusage)
{
    int64_t ret = sfk_syscall4(SYS_wait4, pid, (int64_t)status, options, (int64_t)rusage);
    SET_ERRNO(ret);
    return (pid_t)ret;
}

int kill(pid_t pid, int sig)
{
    int64_t ret = sfk_syscall2(SYS_kill, pid, sig);
    SET_ERRNO(ret);
    return (int)ret;
}

int sched_yield(void)
{
    int64_t ret = sfk_syscall0(SYS_sched_yield);
    SET_ERRNO(ret);
    return (int)ret;
}

/* Memory */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    int64_t ret = sfk_syscall6(SYS_mmap, (int64_t)addr, length, prot, flags, fd, offset);
    if ((uint64_t)ret > 0xFFFFFFFFFFFFF000ULL) {
        sfk_errno = -(int)ret;
        return MAP_FAILED;
    }
    return (void *)ret;
}

int munmap(void *addr, size_t length)
{
    int64_t ret = sfk_syscall2(SYS_munmap, (int64_t)addr, length);
    SET_ERRNO(ret);
    return (int)ret;
}

int mprotect(void *addr, size_t len, int prot)
{
    int64_t ret = sfk_syscall3(SYS_mprotect, (int64_t)addr, len, prot);
    SET_ERRNO(ret);
    return (int)ret;
}

/* Time */
int clock_gettime(int clk_id, struct timespec *tp)
{
    int64_t ret = sfk_syscall2(SYS_clock_gettime, clk_id, (int64_t)tp);
    SET_ERRNO(ret);
    return (int)ret;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    int64_t ret = sfk_syscall2(SYS_gettimeofday, (int64_t)tv, (int64_t)tz);
    SET_ERRNO(ret);
    return (int)ret;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    int64_t ret = sfk_syscall2(SYS_nanosleep, (int64_t)req, (int64_t)rem);
    SET_ERRNO(ret);
    return (int)ret;
}

/* Signal */
int rt_sigaction(int sig, const void *act, void *oact, size_t sigsetsize)
{
    int64_t ret = sfk_syscall4(SYS_rt_sigaction, sig, (int64_t)act, (int64_t)oact, sigsetsize);
    SET_ERRNO(ret);
    return (int)ret;
}

/* Misc */
int uname(void *buf)
{
    int64_t ret = sfk_syscall1(SYS_uname, (int64_t)buf);
    SET_ERRNO(ret);
    return (int)ret;
}

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags)
{
    int64_t ret = sfk_syscall3(SYS_getrandom, (int64_t)buf, buflen, flags);
    SET_ERRNO(ret);
    return (ssize_t)ret;
}
