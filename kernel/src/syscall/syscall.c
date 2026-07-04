#include "syscall_internal.h"
#include "process.h"
#include "memory.h"
#include "hal.h"
#include "string.h"
#include "serial.h"
#include "errno.h"

/* ========================================================================
 * Syscall name lookup table (for debugging)
 * ======================================================================== */

static const char *syscall_names[] = {
    [0]   = "read",
    [1]   = "write",
    [2]   = "open",
    [3]   = "close",
    [4]   = "stat",
    [5]   = "fstat",
    [6]   = "lstat",
    [7]   = "poll",
    [8]   = "lseek",
    [9]   = "mmap",
    [10]  = "mprotect",
    [11]  = "munmap",
    [12]  = "brk",
    [13]  = "rt_sigaction",
    [14]  = "rt_sigprocmask",
    [15]  = "rt_sigreturn",
    [16]  = "ioctl",
    [17]  = "pread64",
    [18]  = "pwrite64",
    [19]  = "readv",
    [20]  = "writev",
    [21]  = "access",
    [22]  = "pipe",
    [23]  = "select",
    [24]  = "sched_yield",
    [25]  = "mremap",
    [26]  = "msync",
    [27]  = "mincore",
    [28]  = "madvise",
    [29]  = "shmget",
    [30]  = "shmat",
    [31]  = "shmctl",
    [32]  = "dup",
    [33]  = "dup2",
    [34]  = "pause",
    [35]  = "nanosleep",
    [36]  = "getitimer",
    [37]  = "alarm",
    [38]  = "setitimer",
    [39]  = "getpid",
    [40]  = "sendfile",
    [41]  = "socket",
    [42]  = "connect",
    [43]  = "accept",
    [44]  = "sendto",
    [45]  = "recvfrom",
    [46]  = "sendmsg",
    [47]  = "recvmsg",
    [48]  = "shutdown",
    [49]  = "bind",
    [50]  = "listen",
    [51]  = "getsockname",
    [52]  = "getpeername",
    [53]  = "socketpair",
    [54]  = "setsockopt",
    [55]  = "getsockopt",
    [56]  = "clone",
    [57]  = "fork",
    [58]  = "vfork",
    [59]  = "execve",
    [60]  = "exit",
    [61]  = "wait4",
    [62]  = "kill",
    [63]  = "uname",
};

static const int syscall_names_max = sizeof(syscall_names) / sizeof(syscall_names[0]);

const char *syscall_name(uint64_t num)
{
    if (num < (uint64_t)syscall_names_max && syscall_names[num])
        return syscall_names[num];

    switch (num) {
    case SYS_getcwd:       return "getcwd";
    case SYS_chdir:        return "chdir";
    case SYS_fchdir:       return "fchdir";
    case SYS_rename:       return "rename";
    case SYS_mkdir:        return "mkdir";
    case SYS_rmdir:        return "rmdir";
    case SYS_creat:        return "creat";
    case SYS_link:         return "link";
    case SYS_unlink:       return "unlink";
    case SYS_symlink:      return "symlink";
    case SYS_readlink:     return "readlink";
    case SYS_chmod:        return "chmod";
    case SYS_fchmod:       return "fchmod";
    case SYS_chown:        return "chown";
    case SYS_fchown:       return "fchown";
    case SYS_umask:        return "umask";
    case SYS_gettimeofday: return "gettimeofday";
    case SYS_getuid:       return "getuid";
    case SYS_getgid:       return "getgid";
    case SYS_setuid:       return "setuid";
    case SYS_setgid:       return "setgid";
    case SYS_geteuid:      return "geteuid";
    case SYS_getegid:      return "getegid";
    case SYS_getppid:      return "getppid";
    case SYS_arch_prctl:   return "arch_prctl";
    case SYS_set_tid_address: return "set_tid_address";
    case SYS_exit_group:   return "exit_group";
    case SYS_fcntl:        return "fcntl";
    case SYS_fsync:        return "fsync";
    case SYS_fdatasync:    return "fdatasync";
    case SYS_truncate:     return "truncate";
    case SYS_ftruncate:    return "ftruncate";
    case SYS_getdents:     return "getdents";
    case SYS_futex:        return "futex";
    case SYS_time:         return "time";
    case SYS_sysinfo:      return "sysinfo";
    case SYS_syslog:       return "syslog";
    case SYS_clock_gettime: return "clock_gettime";
    case SYS_set_robust_list: return "set_robust_list";
    case SYS_get_robust_list: return "get_robust_list";
    case SYS_gettid:       return "gettid";
    case SYS_epoll_create1: return "epoll_create1";
    case SYS_sfk_register_pkg: return "sfk_register_pkg";
    case SYS_sfk_check_perm:   return "sfk_check_perm";
    case SYS_sfk_request_perm: return "sfk_request_perm";
    case SYS_sfk_get_pkg_info: return "sfk_get_pkg_info";
    case SYS_sfk_list_perms:   return "sfk_list_perms";
    case SYS_reg_open:     return "reg_open";
    case SYS_reg_close:    return "reg_close";
    case SYS_reg_read:     return "reg_read";
    case SYS_reg_write:    return "reg_write";
    case SYS_reg_delete:   return "reg_delete";
    case SYS_reg_list:     return "reg_list";
    case SYS_reg_exists:   return "reg_exists";
    default:               return "unknown";
    }
}

/* ========================================================================
 * Main syscall dispatcher
 * ======================================================================== */

void syscall_handler_c(trap_frame_t *frame)
{
    uint64_t syscall_num = frame->rax;
    int64_t ret;

    /* Debug: log write/exit/exit_group syscalls via serial */
    if (syscall_num == 1 || syscall_num == 60 || syscall_num == 231) {
        serial_puts("[SYSCALL] num=0x");
        serial_put_hex(syscall_num);
        serial_puts(" rdi=0x");
        serial_put_hex(frame->rdi);
        serial_puts("\n");
    }

    switch (syscall_num) {
    /* File I/O */
    case SYS_read:          ret = sys_read(frame);          break;
    case SYS_write:         ret = sys_write(frame);         break;
    case SYS_open:          ret = sys_open(frame);          break;
    case SYS_close:         ret = sys_close(frame);         break;
    case SYS_fstat:         ret = sys_fstat(frame);         break;
    case SYS_lseek:         ret = sys_lseek(frame);         break;
    case SYS_pipe:          ret = sys_pipe(frame);          break;
    case SYS_dup:           ret = sys_dup(frame);           break;
    case SYS_dup2:          ret = sys_dup2(frame);          break;
    case SYS_getcwd:        ret = sys_getcwd(frame);        break;
    case SYS_chdir:         ret = sys_chdir(frame);         break;
    case SYS_mkdir:         ret = sys_mkdir(frame);         break;
    case SYS_rmdir:         ret = sys_rmdir(frame);         break;
    case SYS_unlink:        ret = sys_unlink(frame);        break;
    case SYS_stat:          ret = sys_stat(frame);          break;
    case SYS_lstat:         ret = sys_lstat(frame);         break;
    case SYS_access:        ret = sys_access(frame);        break;
    case SYS_readlink:      ret = sys_readlink(frame);      break;
    case SYS_newfstatat:    ret = sys_newfstatat(frame);    break;
    case SYS_readlinkat:    ret = sys_readlinkat(frame);    break;
    case SYS_pread64:       ret = sys_pread64(frame);       break;
    case SYS_pwrite64:      ret = sys_pwrite64(frame);      break;
    case SYS_getdents:      ret = sys_getdents(frame);      break;
    case SYS_getdents64:    ret = sys_getdents64(frame);    break;
    case SYS_rename:        ret = sys_rename(frame);        break;
    case SYS_chmod:         ret = sys_chmod(frame);         break;
    case SYS_fchmod:        ret = sys_fchmod(frame);        break;
    case SYS_chown:         ret = sys_chown(frame);         break;
    case SYS_fcntl:         ret = sys_fcntl(frame);         break;
    case SYS_flock:         ret = sys_flock(frame);         break;
    case SYS_fsync:         ret = sys_fsync(frame);         break;
    case SYS_fadvise64:     ret = sys_fadvise64(frame);     break;
    case SYS_pipe2:         ret = sys_pipe2(frame);         break;
    case SYS_dup3:          ret = sys_dup3(frame);          break;

    /* Memory */
    case SYS_mmap:          ret = sys_mmap(frame);          break;
    case SYS_brk:           ret = sys_brk(frame);           break;
    case SYS_mprotect:      ret = sys_mprotect(frame);      break;
    case SYS_munmap:        ret = sys_munmap(frame);        break;
    case SYS_madvise:       ret = sys_madvise(frame);       break;
    case SYS_mlock:         ret = sys_mlock(frame);         break;
    case SYS_munlock:       ret = sys_munlock(frame);       break;
    case SYS_mlockall:      ret = sys_mlockall(frame);      break;
    case SYS_munlockall:    ret = sys_munlockall(frame);    break;

    /* Process management */
    case SYS_fork:          ret = sys_fork(frame);          break;
    case SYS_execve:        ret = sys_execve(frame);        break;
    case SYS_exit:          ret = sys_exit(frame);          break;
    case SYS_wait4:         ret = sys_wait4(frame);         break;
    case SYS_kill:          ret = sys_kill(frame);          break;
    case SYS_getpid:        ret = sys_getpid(frame);        break;
    case SYS_gettid:        ret = sys_gettid(frame);        break;
    case SYS_getppid:       ret = sys_getppid(frame);       break;
    case SYS_getuid:        ret = sys_getuid(frame);        break;
    case SYS_getgid:        ret = sys_getgid(frame);        break;
    case SYS_geteuid:       ret = sys_geteuid(frame);       break;
    case SYS_getegid:       ret = sys_getegid(frame);       break;
    case SYS_setuid:        ret = sys_setuid(frame);        break;
    case SYS_setgid:        ret = sys_setgid(frame);        break;
    case SYS_getgroups:     ret = sys_getgroups(frame);     break;
    case SYS_setgroups:     ret = sys_setgroups(frame);     break;
    case SYS_arch_prctl:    ret = sys_arch_prctl(frame);    break;
    case SYS_set_tid_address: ret = sys_set_tid_address(frame); break;
    case SYS_exit_group:    ret = sys_exit_group(frame);    break;

    /* Threading */
    case SYS_clone:         ret = sys_clone(frame);         break;
    case SYS_vfork:         ret = sys_vfork(frame);         break;

    /* Signal handling */
    case SYS_rt_sigaction:  ret = sys_rt_sigaction(frame);  break;
    case SYS_rt_sigprocmask: ret = sys_rt_sigprocmask(frame); break;
    case SYS_rt_sigreturn:  ret = sys_rt_sigreturn(frame);  break;
    case SYS_rt_sigpending: ret = sys_rt_sigpending(frame); break;
    case SYS_sigaltstack:   ret = sys_sigaltstack(frame);   break;
    case SYS_tgkill:        ret = sys_tgkill(frame);        break;
    case SYS_tkill:         ret = sys_tkill(frame);         break;
    case SYS_rt_sigsuspend: ret = sys_rt_sigsuspend(frame); break;

    /* Identity */
    case SYS_getresuid:     ret = sys_getresuid(frame);     break;
    case SYS_getresgid:     ret = sys_getresgid(frame);     break;
    case SYS_capget:        ret = sys_capget(frame);        break;
    case SYS_capset:        ret = sys_capset(frame);        break;
    case SYS_prctl:         ret = sys_prctl(frame);         break;
    case SYS_prlimit64:     ret = sys_prlimit64(frame);     break;

    /* Time */
    case SYS_clock_gettime: ret = sys_clock_gettime(frame); break;
    case SYS_clock_getres:  ret = sys_clock_getres(frame);  break;
    case SYS_nanosleep:     ret = sys_nanosleep(frame);     break;
    case SYS_gettimeofday:  ret = sys_gettimeofday(frame);  break;
    case SYS_time:          ret = sys_time(frame);          break;

    /* Scheduling */
    case SYS_sched_yield:   ret = sys_sched_yield(frame);   break;

    /* Misc */
    case SYS_uname:         ret = sys_uname(frame);         break;
    case SYS_poll:          ret = sys_poll(frame);          break;
    case SYS_getrandom:     ret = sys_getrandom(frame);     break;
    case SYS_futex:         ret = sys_futex(frame);         break;
    case SYS_set_robust_list: ret = sys_set_robust_list(frame); break;
    case SYS_get_robust_list: ret = sys_get_robust_list(frame); break;
    case SYS_sysinfo:       ret = sys_sysinfo(frame);       break;
    case SYS_getrlimit:     ret = sys_getrlimit(frame);     break;
    case SYS_setrlimit:     ret = sys_setrlimit(frame);     break;
    case SYS_membarrier:    ret = sys_membarrier(frame);    break;
    case SYS_rseq:          ret = sys_rseq(frame);          break;

    /* Network syscalls */
    case SYS_socket:        ret = sys_socket(frame);        break;
    case SYS_connect:       ret = sys_connect(frame);       break;
    case SYS_accept:        ret = sys_accept(frame);        break;
    case SYS_bind:          ret = sys_bind(frame);          break;
    case SYS_listen:        ret = sys_listen(frame);        break;
    case SYS_sendto:        ret = sys_sendto(frame);        break;
    case SYS_recvfrom:      ret = sys_recvfrom(frame);      break;
    case SYS_shutdown:      ret = sys_shutdown(frame);       break;
    case SYS_getsockname:   ret = sys_getsockname(frame);   break;
    case SYS_getpeername:   ret = sys_getpeername(frame);   break;
    case SYS_socketpair:    ret = sys_socketpair(frame);    break;
    case SYS_setsockopt:    ret = sys_setsockopt(frame);    break;
    case SYS_getsockopt:    ret = sys_getsockopt(frame);    break;

    /* epoll */
    case SYS_epoll_create:  ret = sys_epoll_create(frame);  break;
    case SYS_epoll_create1: ret = sys_epoll_create1(frame); break;
    case SYS_epoll_ctl:     ret = sys_epoll_ctl(frame);     break;
    case SYS_epoll_wait:    ret = sys_epoll_wait(frame);    break;

    /* eventfd */
    case SYS_eventfd:       ret = sys_eventfd(frame);       break;
    case SYS_eventfd2:      ret = sys_eventfd2(frame);      break;

    /* signalfd */
    case SYS_signalfd:      ret = sys_signalfd(frame);      break;
    case SYS_signalfd4:     ret = sys_signalfd4(frame);     break;

    /* timerfd */
    case SYS_timerfd_create:  ret = sys_timerfd_create(frame);  break;
    case SYS_timerfd_settime: ret = sys_timerfd_settime(frame); break;
    case SYS_timerfd_gettime: ret = sys_timerfd_gettime(frame); break;

    /* inotify */
    case SYS_inotify_init:      ret = sys_inotify_init(frame);      break;
    case SYS_inotify_init1:     ret = sys_inotify_init1(frame);     break;
    case SYS_inotify_add_watch: ret = sys_inotify_add_watch(frame); break;
    case SYS_inotify_rm_watch:  ret = sys_inotify_rm_watch(frame);  break;

    /* SFK-specific syscalls */
    case SYS_sfk_check_perm:   ret = sys_sfk_check_perm(frame);   break;
    case SYS_sfk_get_pkg_info: ret = sys_sfk_get_pkg_info(frame); break;

    /* Registry syscalls */
    case SYS_reg_read:      ret = sys_reg_read(frame);      break;
    case SYS_reg_write:     ret = sys_reg_write(frame);     break;
    case SYS_reg_list:      ret = sys_reg_list(frame);      break;

    /* ioctl - not implemented */
    case SYS_ioctl:         ret = -ENOTTY;                  break;

    /* ppoll stub */
    case SYS_ppoll:         ret = 0;                        break;

    default:
        ret = -ENOSYS;
        break;
    }

    /* Deliver pending signals before returning to user mode */
    process_signal_deliver();

    frame->rax = (uint64_t)ret;
}

/* ========================================================================
 * Public syscall handler (matches process.h declaration)
 * ======================================================================== */

uint64_t syscall_handler(trap_frame_t *frame)
{
    syscall_handler_c(frame);
    return frame->rax;
}

/* ========================================================================
 * Syscall subsystem initialization
 * ======================================================================== */

void syscall_init(void)
{
    /* Allocate per-CPU scratch space for syscall entry.
     * This must be done after memory_init() so alloc_page() works.
     * The syscall entry point uses swapgs + gs:0/gs:8 to access
     * user_rsp and kernel_rsp:
     *   offset 0: saved user RSP (written by syscall entry)
     *   offset 8: kernel RSP (written before entering user mode) */
    static void *syscall_cpu_area = NULL;
    if (!syscall_cpu_area) {
        syscall_cpu_area = alloc_page();
        if (syscall_cpu_area) {
            memset(syscall_cpu_area, 0, PAGE_SIZE);
            printf("[SYSCALL] Per-CPU area at %p\n", syscall_cpu_area);
        } else {
            printf("[SYSCALL] ERROR: failed to allocate per-CPU area!\n");
            return;
        }
    }
    hal_write_msr(MSR_IA32_GS_BASE, 0);
    hal_write_msr(MSR_IA32_KERNEL_GS_BASE, (uint64_t)(uintptr_t)syscall_cpu_area);

    /* Verify */
    uint64_t verify = hal_read_msr(MSR_IA32_KERNEL_GS_BASE);
    printf("[SYSCALL] KERNEL_GS_BASE=%llx\n", (unsigned long long)verify);
}
