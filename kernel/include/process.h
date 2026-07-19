#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include "smp.h"
/* Forward declarations from vfs.h - avoid heavy include */
typedef struct vfs_file vfs_file_t;
#define VFS_MAX_PATH 256

/* ========================================================================
 * Process states
 * ======================================================================== */
#define PROC_UNUSED       0
#define PROC_READY       1
#define PROC_RUNNING     2
#define PROC_BLOCKED     3
#define PROC_ZOMBIE      4

/* ========================================================================
 * Process flags
 * ======================================================================== */
#define PROC_FLAG_KERNEL  0x01   /* Kernel thread */
#define PROC_FLAG_SFK     0x02   /* SFK sandboxed app */
#define PROC_FLAG_DEB     0x04   /* DEB native Linux app */
#define PROC_FLAG_COW     0x08   /* COW (Copy-on-Write) fork */

/* ========================================================================
 * Signal constants (Linux compatible)
 * ======================================================================== */
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20

#define MAX_SIGNAL 32
#define MAX_PENDING_SIGNALS 64

/* ========================================================================
 * Resource limits
 * ======================================================================== */
#define PROC_MAX_FD       256    /* Max file descriptors per process */
#define PROC_MAX_ARGS     128    /* Max argc */
#define PROC_MAX_ENV      128    /* Max env vars */
#define PROC_STACK_SIZE   (4 * 1024 * 1024)  /* 4MB user stack */
#define PROC_KERNEL_STACK (8192)              /* 8KB kernel stack */

/* ========================================================================
 * Trap frame - saved user-mode registers on syscall/interrupt entry
 * ======================================================================== */
typedef struct {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx;
    uint64_t rbx, rbp, rax;
    uint64_t int_num, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) trap_frame_t;

/* ========================================================================
 * Process control block
 * ======================================================================== */
typedef struct process {
    int             pid;           /* Process ID */
    int             ppid;          /* Parent PID */
    int             state;         /* Process state */
    uint32_t        flags;         /* Process flags */
    uint32_t        exit_code;     /* Exit code (for zombie) */

    /* UID/GID (Linux compatible) */
    uint32_t        uid, gid;
    uint32_t        euid, egid;    /* Effective UID/GID */

    /* Scheduling */
    int             priority;      /* Scheduling priority */
    uint64_t        sleep_until;   /* Wake time for blocked processes */
    uint64_t        cpu_time;      /* Total CPU time consumed (ms) */
    int             cpu_id;        /* 当前运行的 CPU index，-1 = 未运行 */
    struct process *next_in_queue; /* 运行队列链接指针 */

    /* Memory */
    uint64_t        pml4;          /* Physical address of PML4 */
    void           *kernel_stack;  /* Kernel stack (page-aligned) */
    uint64_t        entry_point;   /* Program entry point */
    uint64_t        brk;           /* Program break (heap end) */
    uint64_t        stack_top;     /* User stack top */
    uint64_t        mmap_base;     /* Base address for mmap */
    uint64_t        mmap_current;  /* Current mmap allocation pointer */

    /* File descriptors */
    vfs_file_t     *fd_table[PROC_MAX_FD];
    char            cwd[VFS_MAX_PATH];  /* Current working directory */

    /* Signal handling */
    uint64_t        signal_mask;        /* Blocked signals */
    uint64_t        pending_signals;    /* Pending signals */
    uint64_t        signal_handlers[MAX_SIGNAL]; /* Handler addresses (0 = default) */
    uint64_t        signal_flags[MAX_SIGNAL];    /* Signal flags (SA_SIGINFO etc) */
    uint64_t        signal_restorer;    /* sigreturn restorer address */

    /* SFK sandbox permissions (for PROC_FLAG_SFK) */
    uint32_t        sfk_perms;          /* Granted SFK permissions bitmask */
    char            sfk_pkg_id[64];     /* SFK package identifier */

    /* Thread/clone data */
    uint64_t        clear_tid_address;  /* Address to clear on thread exit (set_tid_address) */

    /* Process tree */
    struct process *parent;        /* Parent process */
    struct process *next;          /* Next in process list */
    struct process *child;         /* First child */
    struct process *sibling;       /* Next sibling */

    /* Saved context for kernel threads */
    trap_frame_t   *trap_frame;    /* Pointer to saved trap frame on kernel stack */
    uint64_t        kernel_rsp;    /* Saved kernel RSP for context switch */
} process_t;

/* ========================================================================
 * Scheduler API
 * ======================================================================== */

/* Initialize process subsystem */
void process_init(void);

/* Create a kernel thread */
process_t *process_create_kthread(void (*entry)(void *), void *arg);

/* Fork the current process */
int process_fork(void);

/* Execute a new program in the current process */
int process_exec(const char *path, const char *const argv[], const char *const envp[]);

/* Clone the current process (thread creation) */
int process_clone(uint64_t flags, uint64_t child_stack, uint64_t ptid, uint64_t ctid, uint64_t newtls);

/* Exit the current process */
void process_exit(int code) __attribute__((noreturn));

/* Wait for a child process */
int process_wait(int pid, int *status, int options);

/* Get current process */
process_t *process_current(void);

/* Process table lock (multi-core safe) */
extern spinlock_t proc_table_lock;

/* Get process by PID */
process_t *process_get(int pid);

/* Yield the CPU */
void process_yield(void);

/* Put current process to sleep until given time */
void process_sleep(uint64_t ms);

/* Send a signal to a process */
int process_kill(int pid, int sig);

/* Check and deliver pending signals (called before returning to user mode) */
void process_signal_deliver(void);

/* Futex wait/wake */
int process_futex_wait(uint32_t *uaddr, uint32_t val, uint64_t timeout_ms);
int process_futex_wake(uint32_t *uaddr, int count);

/* ========================================================================
 * Scheduler
 * ======================================================================== */

/* Scheduler tick (called from timer) */
void scheduler_tick(void);

/* Schedule next process */
void scheduler_schedule(void);

/* Check and clear the need_reschedule flag */
int need_reschedule_check(void);

/* Start the scheduler (never returns) */
void scheduler_start(void) __attribute__((noreturn));

/* ========================================================================
 * User-mode transition
 * ======================================================================== */

/* Jump to user mode (first time) */
void process_enter_user(trap_frame_t *frame);

/* Build trap frame for new user process */
void process_setup_frame(process_t *proc, uint64_t entry, uint64_t stack,
                         uint64_t arg);

/* ========================================================================
 * Process information
 * ======================================================================== */

/* Get the file descriptor table for the current process */
vfs_file_t **process_get_fd_table(void);

/* Allocate a file descriptor in current process */
int process_alloc_fd(void);

/* COW page fault handler - returns 0 if handled, -1 if not a COW fault */
int process_cow_page_fault(uint64_t fault_addr);

/* Write bytes to user virtual address via page table walk (defined in process_user.c) */
int write_user_mem(process_t *proc, uint64_t vaddr,
                   const void *data, size_t len);

#endif /* PROCESS_H */
