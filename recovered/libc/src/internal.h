#ifndef _LIBC_INTERNAL_H
#define _LIBC_INTERNAL_H

/* SpiritFoxOS libc - Internal header
 * Unified syscall numbers, errno handling, and common includes
 * for all libc source files. */

#include <syscall.h>   /* arch/x86_64/syscall.h — sfk_syscall0-6 inline wrappers */
#include <sys/types.h>
#include <stddef.h>
#include <stdarg.h>
#include <errno.h>

/* ========================================================================
 * Syscall numbers (Linux x86_64 ABI) — single source of truth
 * ======================================================================== */

/* File I/O */
#define SYS_read          0
#define SYS_write         1
#define SYS_open          2
#define SYS_close         3
#define SYS_stat          4
#define SYS_fstat         5
#define SYS_lstat         6
#define SYS_poll          7
#define SYS_lseek         8
#define SYS_mmap          9
#define SYS_mprotect     10
#define SYS_munmap      11
#define SYS_brk         12
#define SYS_rt_sigaction 13
#define SYS_rt_sigprocmask 14
#define SYS_ioctl       16
#define SYS_pread64     17
#define SYS_pwrite64    18
#define SYS_access      21
#define SYS_pipe        22
#define SYS_select      23
#define SYS_sched_yield 24
#define SYS_dup         32
#define SYS_dup2        33
#define SYS_nanosleep   35
#define SYS_getpid      39
#define SYS_socket      41
#define SYS_connect     42
#define SYS_accept      43
#define SYS_sendto      44
#define SYS_recvfrom    45
#define SYS_clone       56
#define SYS_fork        57
#define SYS_vfork       58
#define SYS_execve      59
#define SYS_exit        60
#define SYS_wait4       61
#define SYS_kill        62
#define SYS_uname       63
#define SYS_fcntl       72
#define SYS_fsync       74
#define SYS_getdents    78
#define SYS_getcwd      79
#define SYS_chdir       80
#define SYS_rename      82
#define SYS_mkdir       83
#define SYS_rmdir       84
#define SYS_creat       85
#define SYS_unlink      87
#define SYS_readlink    89
#define SYS_chmod       90
#define SYS_fchmod      91
#define SYS_chown       92
#define SYS_umask       95
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
#define SYS_getdents64 217
#define SYS_time       201
#define SYS_futex      202
#define SYS_clock_gettime 228
#define SYS_clock_getres  229
#define SYS_exit_group 231
#define SYS_getrandom  318

/* ========================================================================
 * Errno handling
 * ======================================================================== */

/* Global errno variable — defined in syscall.c, non-static */
extern int sfk_errno;

#define SET_ERRNO(ret) do { \
    if ((ret) < 0) { sfk_errno = -(int)(ret); ret = -1; } \
    else { sfk_errno = 0; } \
} while(0)

#endif /* _LIBC_INTERNAL_H */
