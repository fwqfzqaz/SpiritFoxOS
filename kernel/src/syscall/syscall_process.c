#include "syscall_internal.h"
#include "process.h"
#include "vfs.h"
#include "memory.h"
#include "hal.h"
#include "string.h"
#include "errno.h"
#include "clone.h"
#include "mmu.h"

/* ========================================================================
 * arch_prctl sub-functions
 * ======================================================================== */

#define ARCH_SET_GS   0x1001
#define ARCH_SET_FS   0x1002
#define ARCH_GET_FS   0x1003
#define ARCH_GET_GS   0x1004

/* ========================================================================
 * Process/thread/signal syscalls
 * ======================================================================== */

int64_t sys_fork(trap_frame_t *frame)
{
    (void)frame;
    return process_fork();
}

int64_t sys_execve(trap_frame_t *frame)
{
    const char *path = (const char *)frame->rdi;
    const char *const *argv = (const char *const *)frame->rsi;
    const char *const *envp = (const char *const *)frame->rdx;

    int ret = process_exec(path, argv, envp);
    if (ret < 0)
        return -ENOENT;

    /* process_exec does not return on success */
    __builtin_unreachable();
}

int64_t sys_exit(trap_frame_t *frame)
{
    int error_code = (int)frame->rdi;
    process_exit(error_code);
    __builtin_unreachable();
}

int64_t sys_wait4(trap_frame_t *frame)
{
    int pid = (int)frame->rdi;
    int *status = (int *)frame->rsi;
    int options = (int)frame->rdx;
    (void)status;
    return process_wait(pid, status, options);
}

int64_t sys_kill(trap_frame_t *frame)
{
    int pid = (int)frame->rdi;
    int sig = (int)frame->rsi;
    return process_kill(pid, sig);
}

int64_t sys_getpid(trap_frame_t *frame)
{
    (void)frame;
    process_t *proc = process_current();
    return proc ? proc->pid : -ESRCH;
}

int64_t sys_getppid(trap_frame_t *frame)
{
    (void)frame;
    process_t *proc = process_current();
    return proc ? proc->ppid : -ESRCH;
}

int64_t sys_getuid(trap_frame_t *frame)
{
    (void)frame;
    process_t *proc = process_current();
    return proc ? (int64_t)proc->uid : -ESRCH;
}

int64_t sys_getgid(trap_frame_t *frame)
{
    (void)frame;
    process_t *proc = process_current();
    return proc ? (int64_t)proc->gid : -ESRCH;
}

int64_t sys_geteuid(trap_frame_t *frame)
{
    (void)frame;
    process_t *proc = process_current();
    return proc ? (int64_t)proc->euid : -ESRCH;
}

int64_t sys_getegid(trap_frame_t *frame)
{
    (void)frame;
    process_t *proc = process_current();
    return proc ? (int64_t)proc->egid : -ESRCH;
}

int64_t sys_setuid(trap_frame_t *frame)
{
    uint32_t uid = (uint32_t)frame->rdi;
    process_t *proc = process_current();
    if (!proc)
        return -ESRCH;

    /* Only root (uid 0) can set uid */
    if (proc->uid != 0 && proc->euid != 0)
        return -EPERM;

    proc->uid = uid;
    proc->euid = uid;
    return 0;
}

int64_t sys_setgid(trap_frame_t *frame)
{
    uint32_t gid = (uint32_t)frame->rdi;
    process_t *proc = process_current();
    if (!proc)
        return -ESRCH;

    /* Only root (uid 0) can set gid */
    if (proc->uid != 0 && proc->euid != 0)
        return -EPERM;

    proc->gid = gid;
    proc->egid = gid;
    return 0;
}

int64_t sys_getgroups(trap_frame_t *frame)
{
    int size = (int)frame->rdi;
    uint32_t *list = (uint32_t *)frame->rsi;
    (void)list;
    (void)size;
    /* No supplementary groups */
    return 0;
}

int64_t sys_setgroups(trap_frame_t *frame)
{
    (void)frame;
    /* No supplementary groups - root only, ignore */
    return 0;
}

int64_t sys_clone(trap_frame_t *frame)
{
    uint64_t flags      = frame->rdi;
    uint64_t child_stack = frame->rsi;
    uint64_t ptid       = frame->rdx;
    uint64_t ctid       = frame->r10;
    uint64_t newtls     = frame->r8;

    return process_clone(flags, child_stack, ptid, ctid, newtls);
}

int64_t sys_vfork(trap_frame_t *frame)
{
    return process_clone(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND,
                         0, 0, 0, 0);
}

int64_t sys_rt_sigaction(trap_frame_t *frame)
{
    int sig = (int)frame->rdi;
    const linux_sigaction_t *act = (const linux_sigaction_t *)frame->rsi;
    linux_sigaction_t *oact = (linux_sigaction_t *)frame->rdx;
    size_t sigsetsize = (size_t)frame->r10;

    (void)sigsetsize;

    if (sig < 1 || sig >= MAX_SIGNAL || sig == SIGKILL || sig == SIGSTOP)
        return -EINVAL;

    process_t *proc = process_current();
    if (!proc)
        return -ESRCH;

    /* Return old action if requested */
    if (oact) {
        oact->sa_handler = proc->signal_handlers[sig - 1];
        oact->sa_flags   = proc->signal_flags[sig - 1];
        oact->sa_mask    = proc->signal_mask;
        oact->sa_restorer = proc->signal_restorer;
    }

    /* Set new action if provided */
    if (act) {
        proc->signal_handlers[sig - 1] = act->sa_handler;
        proc->signal_flags[sig - 1]    = act->sa_flags;
        if (act->sa_restorer)
            proc->signal_restorer = act->sa_restorer;
    }

    return 0;
}

int64_t sys_rt_sigprocmask(trap_frame_t *frame)
{
    int how = (int)frame->rdi;
    const uint64_t *set    = (const uint64_t *)frame->rsi;
    uint64_t       *oldset = (uint64_t *)frame->rdx;
    size_t sigsetsize = (size_t)frame->r10;

    (void)sigsetsize;

    process_t *proc = process_current();
    if (!proc)
        return -ESRCH;

    /* Return old mask if requested */
    if (oldset)
        *oldset = proc->signal_mask;

    /* Set new mask if provided */
    if (set) {
        switch (how) {
        case 0: /* SIG_BLOCK */
            proc->signal_mask |= *set;
            break;
        case 1: /* SIG_UNBLOCK */
            proc->signal_mask &= ~(*set);
            break;
        case 2: /* SIG_SETMASK */
            proc->signal_mask = *set;
            break;
        default:
            return -EINVAL;
        }

        /* Never allow blocking SIGKILL and SIGSTOP */
        proc->signal_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    }

    return 0;
}

int64_t sys_rt_sigreturn(trap_frame_t *frame)
{
    process_t *proc = process_current();
    if (!proc || !proc->trap_frame)
        return -EINVAL;

    /* The signal handler pushed a trap_frame_t copy on the user stack,
     * followed by the signal number (8 bytes).  The current user RSP
     * points after the return address (which was popped by ret).
     *
     * Layout was: [ret_addr][sig_num(8)][trap_frame_t copy]
     * After handler ret: RSP points to [sig_num(8)][trap_frame_t copy]
     */

    uint64_t user_sp = frame->rsp;

    /* Skip signal number */
    user_sp += 8;

    /* Read the saved trap frame from user stack using mmu_virt_to_phys */
    uint64_t phys = mmu_virt_to_phys(proc->pml4, user_sp);
    if (phys == 0)
        return -EFAULT;

    trap_frame_t *saved_tf = (trap_frame_t *)(uintptr_t)phys;

    /* Restore the original trap frame */
    *frame = *saved_tf;

    /* Adjust RSP past the saved trap frame */
    frame->rsp = user_sp + sizeof(trap_frame_t);

    /* Return the original rax value */
    return (int64_t)frame->rax;
}

int64_t sys_rt_sigpending(trap_frame_t *frame)
{
    uint64_t *set = (uint64_t *)frame->rdi;
    process_t *proc = process_current();
    if (!proc)
        return -ESRCH;
    if (set)
        *set = proc->pending_signals;
    return 0;
}

int64_t sys_arch_prctl(trap_frame_t *frame)
{
    int code = (int)frame->rdi;
    uint64_t addr = frame->rsi;

    switch (code) {
    case ARCH_SET_FS:
        hal_write_msr(MSR_IA32_FS_BASE, addr);
        return 0;
    case ARCH_GET_FS:
        return (int64_t)hal_read_msr(MSR_IA32_FS_BASE);
    case ARCH_SET_GS:
        hal_write_msr(MSR_IA32_GS_BASE, addr);
        return 0;
    case ARCH_GET_GS:
        return (int64_t)hal_read_msr(MSR_IA32_GS_BASE);
    default:
        return -EINVAL;
    }
}

int64_t sys_set_tid_address(trap_frame_t *frame)
{
    uint64_t tidptr = frame->rdi;
    process_t *proc = process_current();
    if (!proc)
        return -ESRCH;

    proc->clear_tid_address = tidptr;
    return proc->pid;
}

int64_t sys_gettid(trap_frame_t *frame)
{
    (void)frame;
    process_t *proc = process_current();
    return proc ? proc->pid : -ESRCH;
}

int64_t sys_exit_group(trap_frame_t *frame)
{
    int error_code = (int)frame->rdi;
    process_exit(error_code);
    __builtin_unreachable();
}

int64_t sys_tgkill(trap_frame_t *frame)
{
    int tgid = (int)frame->rdi;
    int tid = (int)frame->rsi;
    int sig = (int)frame->rdx;
    (void)tgid;
    /* For now, treat like kill */
    return process_kill(tid, sig);
}

int64_t sys_tkill(trap_frame_t *frame)
{
    int tid = (int)frame->rdi;
    int sig = (int)frame->rsi;
    return process_kill(tid, sig);
}

int64_t sys_sigaltstack(trap_frame_t *frame)
{
    (void)frame;
    /* Stub: sigaltstack not fully implemented */
    return 0;
}

int64_t sys_getresuid(trap_frame_t *frame)
{
    uint32_t *ruid = (uint32_t *)frame->rdi;
    uint32_t *euid = (uint32_t *)frame->rsi;
    uint32_t *suid = (uint32_t *)frame->rdx;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (ruid) *ruid = proc->uid;
    if (euid) *euid = proc->euid;
    if (suid) *suid = proc->uid;
    return 0;
}

int64_t sys_getresgid(trap_frame_t *frame)
{
    uint32_t *rgid = (uint32_t *)frame->rdi;
    uint32_t *egid = (uint32_t *)frame->rsi;
    uint32_t *sgid = (uint32_t *)frame->rdx;
    process_t *proc = process_current();
    if (!proc) return -ESRCH;
    if (rgid) *rgid = proc->gid;
    if (egid) *egid = proc->egid;
    if (sgid) *sgid = proc->gid;
    return 0;
}

int64_t sys_capget(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

int64_t sys_capset(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

int64_t sys_prctl(trap_frame_t *frame)
{
    int option = (int)frame->rdi;
    (void)option;
    /* Stub: prctl is used for various process operations.
     * JVM uses PR_SET_NAME, PR_GET_NAME, PR_SET_PTRACER, etc.
     * Return 0 for most options to not break JVM. */
    return 0;
}

int64_t sys_prlimit64(trap_frame_t *frame)
{
    /* Stub: resource limits not implemented */
    (void)frame;
    return 0;
}
