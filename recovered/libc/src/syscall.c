/* SpiritFoxOS libc - Syscall wrappers
 * Implements POSIX syscall wrappers using the Linux-compatible
 * syscall ABI provided by the SpiritFoxOS kernel.
 *
 * All SYS_* macros come from internal.h (single source of truth).
 */

#include "internal.h"
#include <fcntl.h>
#include <mman.h>
#include <time.h>
#include <sys/stat.h>
#include <stdarg.h>

/* ========================================================================
 * errno — global variable, non-static for internal.h
 * ======================================================================== */
int sfk_errno = 0;
int *__errno_location(void) { return &sfk_errno; }

/* ========================================================================
 * File I/O
 * ======================================================================== */

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

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    int64_t arg = va_arg(ap, int64_t);
    va_end(ap);
    int64_t ret = sfk_syscall3(SYS_ioctl, fd, (int64_t)request, arg);
    SET_ERRNO(ret);
    return (int)ret;
}

/* ========================================================================
 * File status (struct stat defined in sys/stat.h)
 * ======================================================================== */

int stat(const char *pathname, struct stat *statbuf)
{
    int64_t ret = sfk_syscall2(SYS_stat, (int64_t)pathname, (int64_t)statbuf);
    SET_ERRNO(ret);
    return (int)ret;
}

int fstat(int fd, struct stat *statbuf)
{
    int64_t ret = sfk_syscall2(SYS_fstat, fd, (int64_t)statbuf);
    SET_ERRNO(ret);
    return (int)ret;
}

int lstat(const char *pathname, struct stat *statbuf)
{
    int64_t ret = sfk_syscall2(SYS_lstat, (int64_t)pathname, (int64_t)statbuf);
    SET_ERRNO(ret);
    return (int)ret;
}

/* ========================================================================
 * Directory operations
 * ======================================================================== */

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

int fchmod(int fd, mode_t mode)
{
    int64_t ret = sfk_syscall2(SYS_fchmod, fd, mode);
    SET_ERRNO(ret);
    return (int)ret;
}

int chown(const char *path, uid_t owner, gid_t group)
{
    int64_t ret = sfk_syscall3(SYS_chown, (int64_t)path, owner, group);
    SET_ERRNO(ret);
    return (int)ret;
}

mode_t umask(mode_t mask)
{
    return (mode_t)sfk_syscall1(SYS_umask, mask);
}

int access(const char *pathname, int mode)
{
    int64_t ret = sfk_syscall2(SYS_access, (int64_t)pathname, mode);
    SET_ERRNO(ret);
    return (int)ret;
}

ssize_t getdents64(int fd, void *dirp, size_t count)
{
    int64_t ret = sfk_syscall3(SYS_getdents64, fd, (int64_t)dirp, count);
    SET_ERRNO(ret);
    return (ssize_t)ret;
}

/* ========================================================================
 * Process management
 * ======================================================================== */

pid_t getpid(void)
{
    return (pid_t)sfk_syscall0(SYS_getpid);
}

pid_t getppid(void)
{
    return (pid_t)sfk_syscall0(SYS_getppid);
}

uid_t getuid(void)  { return (uid_t)sfk_syscall0(SYS_getuid); }
uid_t geteuid(void) { return (uid_t)sfk_syscall0(SYS_geteuid); }
gid_t getgid(void)  { return (gid_t)sfk_syscall0(SYS_getgid); }
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

pid_t wait4(pid_t pid, int *status, int options, void *rusage)
{
    int64_t ret = sfk_syscall4(SYS_wait4, pid, (int64_t)status, options, (int64_t)rusage);
    SET_ERRNO(ret);
    return (pid_t)ret;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    return wait4(pid, status, options, NULL);
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

/* ========================================================================
 * Memory management
 * ======================================================================== */

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

/* ========================================================================
 * Time
 * ======================================================================== */

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

time_t time(time_t *tloc)
{
    int64_t ret = sfk_syscall1(SYS_time, tloc ? (int64_t)tloc : 0);
    if (ret < 0) { sfk_errno = -(int)ret; return -1; }
    if (tloc) *tloc = (time_t)ret;
    return (time_t)ret;
}

/* ========================================================================
 * Signal
 * ======================================================================== */

int rt_sigaction(int sig, const void *act, void *oact, size_t sigsetsize)
{
    int64_t ret = sfk_syscall4(SYS_rt_sigaction, sig, (int64_t)act, (int64_t)oact, sigsetsize);
    SET_ERRNO(ret);
    return (int)ret;
}

/* ========================================================================
 * Miscellaneous
 * ======================================================================== */

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

int isatty(int fd)
{
    /* Simple implementation: try ioctl TIOCGETD.
     * Kernel ioctl is not yet implemented, so always return 0. */
    (void)fd;
    return 0;
}
