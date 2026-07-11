#ifndef SYSCALL_INTERNAL_H
#define SYSCALL_INTERNAL_H

#include "syscall.h"

/* 所有系统调用处理函数签名 - 分发器和实现文件共用 */
typedef int64_t (*syscall_handler_t)(trap_frame_t *frame);

/* File I/O syscalls (syscall_file.c) */
int64_t sys_read(trap_frame_t *frame);
int64_t sys_write(trap_frame_t *frame);
int64_t sys_open(trap_frame_t *frame);
int64_t sys_close(trap_frame_t *frame);
int64_t sys_fstat(trap_frame_t *frame);
int64_t sys_lseek(trap_frame_t *frame);
int64_t sys_pipe(trap_frame_t *frame);
int64_t sys_dup(trap_frame_t *frame);
int64_t sys_dup2(trap_frame_t *frame);
int64_t sys_getcwd(trap_frame_t *frame);
int64_t sys_chdir(trap_frame_t *frame);
int64_t sys_mkdir(trap_frame_t *frame);
int64_t sys_rmdir(trap_frame_t *frame);
int64_t sys_unlink(trap_frame_t *frame);
int64_t sys_stat(trap_frame_t *frame);
int64_t sys_lstat(trap_frame_t *frame);
int64_t sys_access(trap_frame_t *frame);
int64_t sys_readlink(trap_frame_t *frame);
int64_t sys_newfstatat(trap_frame_t *frame);
int64_t sys_readlinkat(trap_frame_t *frame);
int64_t sys_pread64(trap_frame_t *frame);
int64_t sys_pwrite64(trap_frame_t *frame);
int64_t sys_getdents(trap_frame_t *frame);
int64_t sys_getdents64(trap_frame_t *frame);
int64_t sys_rename(trap_frame_t *frame);
int64_t sys_chmod(trap_frame_t *frame);
int64_t sys_fchmod(trap_frame_t *frame);
int64_t sys_chown(trap_frame_t *frame);
int64_t sys_fcntl(trap_frame_t *frame);
int64_t sys_flock(trap_frame_t *frame);
int64_t sys_fsync(trap_frame_t *frame);
int64_t sys_fadvise64(trap_frame_t *frame);
int64_t sys_pipe2(trap_frame_t *frame);
int64_t sys_dup3(trap_frame_t *frame);

/* 进程/线程/信号系统调用 (syscall_process.c) */
int64_t sys_fork(trap_frame_t *frame);
int64_t sys_execve(trap_frame_t *frame);
int64_t sys_exit(trap_frame_t *frame);
int64_t sys_wait4(trap_frame_t *frame);
int64_t sys_kill(trap_frame_t *frame);
int64_t sys_getpid(trap_frame_t *frame);
int64_t sys_gettid(trap_frame_t *frame);
int64_t sys_getppid(trap_frame_t *frame);
int64_t sys_getuid(trap_frame_t *frame);
int64_t sys_getgid(trap_frame_t *frame);
int64_t sys_geteuid(trap_frame_t *frame);
int64_t sys_getegid(trap_frame_t *frame);
int64_t sys_setuid(trap_frame_t *frame);
int64_t sys_setgid(trap_frame_t *frame);
int64_t sys_getgroups(trap_frame_t *frame);
int64_t sys_setgroups(trap_frame_t *frame);
int64_t sys_clone(trap_frame_t *frame);
int64_t sys_vfork(trap_frame_t *frame);
int64_t sys_rt_sigaction(trap_frame_t *frame);
int64_t sys_rt_sigprocmask(trap_frame_t *frame);
int64_t sys_rt_sigreturn(trap_frame_t *frame);
int64_t sys_rt_sigpending(trap_frame_t *frame);
int64_t sys_arch_prctl(trap_frame_t *frame);
int64_t sys_set_tid_address(trap_frame_t *frame);
int64_t sys_exit_group(trap_frame_t *frame);
int64_t sys_tgkill(trap_frame_t *frame);
int64_t sys_tkill(trap_frame_t *frame);
int64_t sys_sigaltstack(trap_frame_t *frame);
int64_t sys_getresuid(trap_frame_t *frame);
int64_t sys_getresgid(trap_frame_t *frame);
int64_t sys_capget(trap_frame_t *frame);
int64_t sys_capset(trap_frame_t *frame);
int64_t sys_prctl(trap_frame_t *frame);
int64_t sys_prlimit64(trap_frame_t *frame);

/* 内存系统调用 (syscall_mem.c) */
int64_t sys_mmap(trap_frame_t *frame);
int64_t sys_brk(trap_frame_t *frame);
int64_t sys_mprotect(trap_frame_t *frame);
int64_t sys_munmap(trap_frame_t *frame);
int64_t sys_madvise(trap_frame_t *frame);
int64_t sys_mlock(trap_frame_t *frame);
int64_t sys_munlock(trap_frame_t *frame);
int64_t sys_mlockall(trap_frame_t *frame);
int64_t sys_munlockall(trap_frame_t *frame);

/* 网络系统调用 (syscall_net.c) */
int64_t sys_socket(trap_frame_t *frame);
int64_t sys_connect(trap_frame_t *frame);
int64_t sys_accept(trap_frame_t *frame);
int64_t sys_bind(trap_frame_t *frame);
int64_t sys_listen(trap_frame_t *frame);
int64_t sys_sendto(trap_frame_t *frame);
int64_t sys_recvfrom(trap_frame_t *frame);
int64_t sys_shutdown(trap_frame_t *frame);
int64_t sys_getsockname(trap_frame_t *frame);
int64_t sys_getpeername(trap_frame_t *frame);
int64_t sys_socketpair(trap_frame_t *frame);
int64_t sys_setsockopt(trap_frame_t *frame);
int64_t sys_getsockopt(trap_frame_t *frame);

/* 杂项系统调用 (syscall_misc.c) */
int64_t sys_uname(trap_frame_t *frame);
int64_t sys_gettimeofday(trap_frame_t *frame);
int64_t sys_clock_gettime(trap_frame_t *frame);
int64_t sys_nanosleep(trap_frame_t *frame);
int64_t sys_time(trap_frame_t *frame);
int64_t sys_sched_yield(trap_frame_t *frame);
int64_t sys_poll(trap_frame_t *frame);
int64_t sys_getrandom(trap_frame_t *frame);
int64_t sys_futex(trap_frame_t *frame);
int64_t sys_set_robust_list(trap_frame_t *frame);
int64_t sys_get_robust_list(trap_frame_t *frame);
int64_t sys_sysinfo(trap_frame_t *frame);
int64_t sys_epoll_create(trap_frame_t *frame);
int64_t sys_epoll_create1(trap_frame_t *frame);
int64_t sys_epoll_ctl(trap_frame_t *frame);
int64_t sys_epoll_wait(trap_frame_t *frame);
int64_t sys_eventfd(trap_frame_t *frame);
int64_t sys_eventfd2(trap_frame_t *frame);
int64_t sys_signalfd(trap_frame_t *frame);
int64_t sys_signalfd4(trap_frame_t *frame);
int64_t sys_timerfd_create(trap_frame_t *frame);
int64_t sys_timerfd_settime(trap_frame_t *frame);
int64_t sys_timerfd_gettime(trap_frame_t *frame);
int64_t sys_inotify_init(trap_frame_t *frame);
int64_t sys_inotify_init1(trap_frame_t *frame);
int64_t sys_inotify_add_watch(trap_frame_t *frame);
int64_t sys_inotify_rm_watch(trap_frame_t *frame);
int64_t sys_dl_iterate_phdr(trap_frame_t *frame);
int64_t sys_getauxval(trap_frame_t *frame);
int64_t sys_membarrier(trap_frame_t *frame);
int64_t sys_rseq(trap_frame_t *frame);
int64_t sys_getrlimit(trap_frame_t *frame);
int64_t sys_setrlimit(trap_frame_t *frame);
int64_t sys_sfk_check_perm(trap_frame_t *frame);
int64_t sys_sfk_get_pkg_info(trap_frame_t *frame);
int64_t sys_reg_read(trap_frame_t *frame);
int64_t sys_reg_write(trap_frame_t *frame);
int64_t sys_reg_list(trap_frame_t *frame);
int64_t sys_clock_getres(trap_frame_t *frame);
int64_t sys_rt_sigsuspend(trap_frame_t *frame);

#endif /* SYSCALL_INTERNAL_H */
