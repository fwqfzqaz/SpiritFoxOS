#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "process.h"
#include "sfk_perms.h"

/* ========================================================================
 * Linux x86-64 syscall numbers (from asm/unistd_64.h)
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
#define SYS_munmap       11
#define SYS_brk          12
#define SYS_ioctl        16
#define SYS_pread64      17
#define SYS_pwrite64     18
#define SYS_readv        19
#define SYS_writev       20
#define SYS_access       21
#define SYS_pipe         22
#define SYS_select       23
#define SYS_dup          32
#define SYS_dup2         33
#define SYS_fcntl        72
#define SYS_fsync        74
#define SYS_fdatasync    75
#define SYS_truncate     76
#define SYS_ftruncate    77
#define SYS_getdents     78
#define SYS_getcwd       79
#define SYS_chdir        80
#define SYS_fchdir       81
#define SYS_rename       82
#define SYS_mkdir        83
#define SYS_rmdir        84
#define SYS_creat        85
#define SYS_link         86
#define SYS_unlink       87
#define SYS_symlink      88
#define SYS_readlink     89
#define SYS_chmod        90
#define SYS_fchmod       91
#define SYS_chown        92
#define SYS_fchown       93
#define SYS_umask        95

/* Process management */
#define SYS_fork        57
#define SYS_vfork       58
#define SYS_execve      59
#define SYS_exit        60
#define SYS_wait4       61
#define SYS_kill        62
#define SYS_getpid      39
#define SYS_gettid     186
#define SYS_getppid    110
#define SYS_getuid     102
#define SYS_getgid     104
#define SYS_geteuid    107
#define SYS_getegid    108
#define SYS_setuid     105
#define SYS_setgid     106
#define SYS_getgroups  115
#define SYS_setgroups  116
#define SYS_clone      56
#define SYS_futex      202
#define SYS_nanosleep  35
#define SYS_alarm      37
#define SYS_setitimer  38
#define SYS_getpid     39
#define SYS_sendfile   40
#define SYS_sched_yield 24

/* Signal handling */
#define SYS_rt_sigaction    13
#define SYS_rt_sigprocmask  14
#define SYS_rt_sigreturn    15
#define SYS_rt_sigpending   127
#define SYS_rt_sigsuspend   130
#define SYS_sigaltstack     131

/* Memory */
#define SYS_mincore     27
#define SYS_madvise     28
#define SYS_shmget      29
#define SYS_shmat       30
#define SYS_shmctl      31

/* Network */
#define SYS_socket      41
#define SYS_connect     42
#define SYS_accept      43
#define SYS_sendto      44
#define SYS_recvfrom    45
#define SYS_sendmsg     46
#define SYS_recvmsg     47
#define SYS_shutdown    48
#define SYS_bind        49
#define SYS_listen      50
#define SYS_getsockname 51
#define SYS_getpeername 52
#define SYS_socketpair  53
#define SYS_setsockopt  54
#define SYS_getsockopt  55

/* Time */
#define SYS_clock_gettime  228
#define SYS_clock_getres   229
#define SYS_gettimeofday   96
#define SYS_settimeofday   97
#define SYS_time           201

/* System info */
#define SYS_uname      63
#define SYS_sysinfo    99
#define SYS_syslog     103

/* Linux-specific */
#define SYS_arch_prctl  158
#define SYS_set_tid_address  218
#define SYS_exit_group  231
#define SYS_set_robust_list  273
#define SYS_get_robust_list  274
#define SYS_prctl       157
#define SYS_getrandom   318
#define SYS_readlinkat  267
#define SYS_newfstatat  262
#define SYS_fstat       5
#define SYS_stat        4
#define SYS_lstat       6
#define SYS_access      21
#define SYS_readlink    89
#define SYS_fcntl       72
#define SYS_flock       73
#define SYS_fsync       74
#define SYS_fadvise64   221
#define SYS_getdents    78
#define SYS_getdents64  217
#define SYS_pipe2       293
#define SYS_dup3        292
#define SYS_poll        7
#define SYS_ppoll       271
#define SYS_munmap      11
#define SYS_mprotect    10
#define SYS_sigaltstack 131
#define SYS_tgkill      234
#define SYS_tkill       200
#define SYS_prlimit64   302
#define SYS_getresuid   118
#define SYS_getresgid   119
#define SYS_capget      125
#define SYS_capset      126

/* epoll */
#define SYS_epoll_create    213
#define SYS_epoll_create1   291
#define SYS_epoll_ctl       233
#define SYS_epoll_wait      232

/* eventfd */
#define SYS_eventfd         284
#define SYS_eventfd2        290

/* signalfd */
#define SYS_signalfd        282
#define SYS_signalfd4       289

/* timerfd */
#define SYS_timerfd_create  283
#define SYS_timerfd_settime 286
#define SYS_timerfd_gettime 287

/* inotify */
#define SYS_inotify_init    253
#define SYS_inotify_init1   294
#define SYS_inotify_add_watch 254
#define SYS_inotify_rm_watch  255

/* resource limits */
#define SYS_getrlimit       97
#define SYS_setrlimit       160

/* mlock */
#define SYS_mlock           149
#define SYS_munlock         150
#define SYS_mlockall        151
#define SYS_munlockall      152

/* misc */
#define SYS_membarrier      324
#define SYS_rseq            334

/* ========================================================================
 * Linux-compatible stat structure
 * ======================================================================== */

typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime;
    uint64_t st_atime_nsec;
    uint64_t st_mtime;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
} linux_stat_t;

/* Linux-compatible sigaction */
typedef struct {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_mask;
    uint64_t sa_restorer;
} linux_sigaction_t;

/* ========================================================================
 * Linux-compatible dirent64 structure (for getdents64 syscall)
 * ======================================================================== */

/* d_type values */
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10
#define DT_SOCK    12

typedef struct {
    uint64_t d_ino;           /* Inode number */
    int64_t  d_off;           /* Offset to next dirent */
    uint16_t d_reclen;        /* Length of this linux_dirent64_t */
    uint8_t  d_type;          /* File type (DT_*) */
    char     d_name[];        /* Filename (null-terminated) */
} linux_dirent64_t;

/* ========================================================================
 * SFK-specific syscalls (600-699)
 * ======================================================================== */

#define SYS_sfk_register_pkg    600
#define SYS_sfk_check_perm      601
#define SYS_sfk_request_perm    602
#define SYS_sfk_get_pkg_info    603
#define SYS_sfk_list_perms      604

/* Registry syscalls (700-799) */
#define SYS_reg_open            700
#define SYS_reg_close           701
#define SYS_reg_read            702
#define SYS_reg_write           703
#define SYS_reg_delete          704
#define SYS_reg_list            705
#define SYS_reg_exists          706
#define SYS_reg_transaction_begin  707
#define SYS_reg_transaction_commit 708
#define SYS_reg_transaction_abort   709

/* ========================================================================
 * Syscall handler
 * ======================================================================== */

/* Initialize syscall subsystem */
void syscall_init(void);

/* Main syscall handler (called from assembly) */
uint64_t syscall_handler(trap_frame_t *frame);

/* Get syscall name for debugging */
const char *syscall_name(uint64_t num);

#endif /* SYSCALL_H */
