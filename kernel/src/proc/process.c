/*
 * SpiritFoxOS 进程管理
 *
 * 核心进程创建、生命周期和 COW 支持。
 * 调度器、信号、futex 和用户态代码在单独的文件中：
 *   scheduler.c, signal.c, futex.c, process_user.c
 */

#include "process.h"
#include "gdt.h"
#include "hal.h"
#include "memory.h"
#include "kmalloc.h"
#include "timer.h"
#include "smp.h"
#include "string.h"
#include "vfs.h"
#include "elf64.h"
#include "vga.h"
#include "serial.h"
#include "mmu.h"
#include "clone.h"

/* ========================================================================
 * 常量
 * ======================================================================== */

#define PTE_COW (1ULL << 9)  /* COW 跟踪的自定义位 */

#define MAX_PROCS          256
#define DEFAULT_TIMESLICE  20      /* 1000Hz 下 20ms */
#define KERNEL_STACK_PAGES 2       /* 2 页 = 8KB 内核栈 */

/* ========================================================================
 * 进程表（非 static：与 scheduler.c, signal.c 等共享）
 * ======================================================================== */

process_t proc_table[MAX_PROCS];
process_t *current = NULL;
int need_reschedule = 0;

/* 进程表自旋锁（多核安全）
 * 保护 proc_table 的分配/释放和进程树操作。
 * 锁序：pmm_lock → mmu_lock → proc_table_lock → runqueue_lock → kmalloc_lock */
spinlock_t proc_table_lock;

/* ========================================================================
 * 上下文切换原语 - 位于 isr_stub.S
 * ======================================================================== */

extern void arch_switch_to(uint64_t *old_rsp_ptr, uint64_t new_rsp);

/* 汇编跳板 - 位于 isr_stub.S */
extern void kthread_trampoline_asm(void);

/* ========================================================================
 * PID 分配（仅在此文件中使用）
 * ======================================================================== */

static int alloc_pid(void)
{
    spinlock_acquire(&proc_table_lock);
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED) {
            /* 标记为正在分配，防止其他核重复分配 */
            proc_table[i].state = PROC_READY;
            spinlock_release(&proc_table_lock);
            return i;
        }
    }
    spinlock_release(&proc_table_lock);
    return -1;
}

/* ========================================================================
 * 内核线程跳板
 * ======================================================================== */

/* 从跳板调用，(entry, arg) 在 rdi, rsi 中 */
__attribute__((used)) void kthread_entry(void (*entry)(void *), void *arg)
{
    entry(arg);
    process_exit(0);
    __builtin_unreachable();
}

/* ========================================================================
 * process_init() - 进程初始化
 * ======================================================================== */

void process_init(void)
{
    memset(proc_table, 0, sizeof(proc_table));
    spinlock_init(&proc_table_lock);

    /* 初始化 TSS（BSP 的 cpu_gdts[0].tss 已由 gdt_init() 初始化，
     * 这里同步全局 tss 向后兼容变量） */
    memset(&tss, 0, sizeof(tss));
    tss.iomap_base = sizeof(tss_t);
    /* 同步到 Per-CPU TSS */
    memset(&cpu_gdts[0].tss, 0, sizeof(tss_t));
    cpu_gdts[0].tss.iomap_base = sizeof(tss_t);

    /* Createeateeateethe idleekernel process idleDkernel process idle–kernel process idle/kernel process */
    process_t *p = &proc_table[0];
    p->pid       = 0;
    p->ppid      = -1;
    p->state     = PROC_RUNNING;
    p->flags     = PROC_FLAG_KERNEL;
    p->uid = p->euid = 0;
    p->gid = p->egid = 0;
    p->priority  = DEFAULT_TIMESLICE;
    p->exit_code = 0;
    p->sleep_until = 0;
    p->cpu_time  = 0;
    p->cpu_id    = 0;    /* PID0 运行在 BSP（CPU 0） */
    p->next_in_queue = NULL;

    /* 内存 – 共享内核页表 */
    p->pml4         = hal_read_cr3();
    p->kernel_stack = NULL;       /* PID 0 使用启动栈 */
    p->entry_point  = 0;
    p->brk          = 0;
    p->stack_top    = 0;
    p->mmap_base    = 0;
    p->mmap_current = 0;

    /* File descriptors */
    memset(p->fd_table, 0, sizeof(p->fd_table));
    strcpy(p->cwd, "/");

    /* Signals */
    p->signal_mask      = 0;
    p->pending_signals  = 0;
    memset(p->signal_handlers, 0, sizeof(p->signal_handlers));
    memset(p->signal_flags, 0, sizeof(p->signal_flags));
    p->signal_restorer = 0;

    /* SFK */
    p->sfk_perms = 0;
    memset(p->sfk_pkg_id, 0, sizeof(p->sfk_pkg_id));

    /* 进程树 */
    p->parent  = NULL;
    p->next    = NULL;
    p->child   = NULL;
    p->sibling = NULL;

    /* Saved context – capture current RSP so the first context switch
     * to PID0 doesn't load RSP=0 and triple-fault.  When PID0 is
     * later switched OUT via arch_switch_to, the real RSP will be
     * saved, overwriting this initial value. */
    p->trap_frame  = NULL;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(p->kernel_rsp));

    current = p;

    /* 设置 BSP 的 Per-CPU current_process */
    {
        cpu_local_t *cpu = this_cpu();
        if (cpu)
            cpu->current_process = p;
    }

    gdt_set_tss_rsp0(0, 0);
}

/* ========================================================================
 * process_create_kthread() - 创建内核线程
 * ======================================================================== */

process_t *process_create_kthread(void (*entry)(void *), void *arg)
{
    int pid = alloc_pid();
    if (pid < 0)
        return NULL;

    process_t *p = &proc_table[pid];
    memset(p, 0, sizeof(process_t));

    /* 分配内核栈（2 个连续页） */
    void *stack = alloc_pages(KERNEL_STACK_PAGES);
    if (!stack)
        return NULL;

    uint64_t stack_top = (uint64_t)stack + (KERNEL_STACK_PAGES * PAGE_SIZE);

    /* 填充进程属性 */
    p->pid           = pid;
    p->ppid          = current ? current->pid : 0;
    p->state         = PROC_READY;
    p->flags         = PROC_FLAG_KERNEL;
    p->uid = p->euid = current ? current->uid : 0;
    p->gid = p->egid = current ? current->gid : 0;
    p->priority      = DEFAULT_TIMESLICE;
    p->exit_code     = 0;
    p->sleep_until   = 0;
    p->cpu_time      = 0;

    p->pml4         = hal_read_cr3();
    p->kernel_stack = stack;
    p->entry_point  = (uint64_t)entry;
    p->brk          = 0;
    p->stack_top    = 0;
    p->mmap_base    = 0;
    p->mmap_current = 0;

    memset(p->fd_table, 0, sizeof(p->fd_table));
    strcpy(p->cwd, current ? current->cwd : "/");

    p->signal_mask      = 0;
    p->pending_signals  = 0;
    memset(p->signal_handlers, 0, sizeof(p->signal_handlers));
    memset(p->signal_flags, 0, sizeof(p->signal_flags));
    p->signal_restorer = 0;

    p->sfk_perms = 0;
    memset(p->sfk_pkg_id, 0, sizeof(p->sfk_pkg_id));

    p->parent  = current;
    p->next    = NULL;
    p->child   = NULL;
    p->sibling = NULL;

    /* 链入父进程的子进程列表 */
    if (current) {
        if (!current->child) {
            current->child = p;
        } else {
            process_t *sib = current->child;
            while (sib->sibling)
                sib = sib->sibling;
            sib->sibling = p;
        }
    }

    /* 构建 arch_switch_to 的初始栈帧 */
    uint64_t *sp = (uint64_t *)stack_top;
    *--sp = (uint64_t)arg;                   /* 通过跳板传递给 rsi */
    *--sp = (uint64_t)entry;                 /* 通过跳板传递给 rdi */
    *--sp = (uint64_t)kthread_trampoline_asm;    /* 返回地址 */
    *--sp = 0;  /* rbp */
    *--sp = 0;  /* rbx */
    *--sp = 0;  /* r12 */
    *--sp = 0;  /* r13 */
    *--sp = 0;  /* r14 */
    *--sp = 0;  /* r15 */

    p->kernel_rsp = (uint64_t)sp;
    p->trap_frame = NULL;

    printf("[kthread_create] PID=%d stack=%p krsp=%lx entry=%p arg=%p\n",
           p->pid, stack, (unsigned long)p->kernel_rsp,
           (void *)entry, arg);

    return p;
}

/* ========================================================================
 * process_fork() - 进程分叉
 * ======================================================================== */

int process_fork(void)
{
    if (!current)
        return -1;

    int pid = alloc_pid();
    if (pid < 0)
        return -1;

    process_t *child = &proc_table[pid];
    memset(child, 0, sizeof(process_t));

    /* Copy process attributes */
    child->pid       = pid;
    child->ppid      = current->pid;
    child->state     = PROC_READY;
    child->flags     = current->flags;
    child->uid       = current->uid;
    child->euid      = current->euid;
    child->gid       = current->gid;
    child->egid      = current->egid;
    child->priority  = current->priority;
    child->exit_code = 0;
    child->sleep_until = 0;
    child->cpu_time  = 0;

    /* ---- COW：创建新 PML4，将用户页共享为只读 ---- */
    {
        /* 1. 分配新 PML4 */
        void *new_pml4_page = alloc_page();
        if (!new_pml4_page) {
            child->state = PROC_UNUSED;
            return -1;
        }
        memset(new_pml4_page, 0, PAGE_SIZE);
        uint64_t *dst_pml4 = (uint64_t *)new_pml4_page;
        uint64_t *src_pml4 = (uint64_t *)(uintptr_t)current->pml4;

        /* 2. 复制内核半区（项 256-511） */
        for (int i = 256; i < 512; i++)
            dst_pml4[i] = src_pml4[i];

        /* 3. 复制用户半区（项 0-255）- 共享页表
         *    并将所有用户页标记为只读以实现 COW */
        for (int i = 0; i < 256; i++) {
            if (src_pml4[i] & PTE_PRESENT) {
                /* 通过在两者中映射来共享 PDPT 页 */
                dst_pml4[i] = src_pml4[i];

                /* 遍历 PDPT -> PD -> PT，清除 PTE_WRITABLE，
                 * 设置 PTE_COW */
                uint64_t *pdpt = (uint64_t *)(src_pml4[i] & PTE_ADDR_MASK);
                for (int j = 0; j < 512; j++) {
                    if (!(pdpt[j] & PTE_PRESENT)) continue;
                    if (pdpt[j] & PTE_HUGE) continue;
                    uint64_t *pd = (uint64_t *)(pdpt[j] & PTE_ADDR_MASK);
                    for (int k = 0; k < 512; k++) {
                        if (!(pd[k] & PTE_PRESENT)) continue;
                        if (pd[k] & PTE_HUGE) continue;
                        uint64_t *pt = (uint64_t *)(pd[k] & PTE_ADDR_MASK);
                        for (int l = 0; l < 512; l++) {
                            if (pt[l] & PTE_PRESENT) {
                                pt[l] = (pt[l] & ~PTE_WRITABLE) | PTE_COW;
                            }
                        }
                    }
                }
            }
        }

        child->pml4 = (uint64_t)(uintptr_t)new_pml4_page;

        /* 将父进程和子进程都标记为 COW 进程 */
        child->flags  |= PROC_FLAG_COW;
        current->flags |= PROC_FLAG_COW;
    }

    /* 为子进程分配新的内核栈 */
    void *stack = alloc_pages(KERNEL_STACK_PAGES);
    if (!stack) {
        child->state = PROC_UNUSED;
        return -1;
    }
    child->kernel_stack = stack;

    uint64_t child_stack_top = (uint64_t)stack +
                                (KERNEL_STACK_PAGES * PAGE_SIZE);

    /* Copy file descriptors with refcount increment */
    for (int i = 0; i < PROC_MAX_FD; i++) {
        if (current->fd_table[i]) {
            child->fd_table[i] = current->fd_table[i];
            child->fd_table[i]->refcount++;
        }
    }

    strcpy(child->cwd, current->cwd);

    child->signal_mask     = current->signal_mask;
    child->pending_signals = 0;
    memcpy(child->signal_handlers, current->signal_handlers,
           sizeof(child->signal_handlers));
    memcpy(child->signal_flags, current->signal_flags,
           sizeof(child->signal_flags));
    child->signal_restorer = current->signal_restorer;

    child->sfk_perms = current->sfk_perms;
    strcpy(child->sfk_pkg_id, current->sfk_pkg_id);

    child->entry_point = current->entry_point;
    child->brk         = current->brk;
    child->stack_top   = current->stack_top;
    child->mmap_base   = current->mmap_base;
    child->mmap_current = current->mmap_current;

    /* 进程树 */
    child->parent  = current;
    child->next    = NULL;
    child->child   = NULL;
    child->sibling = NULL;

    if (!current->child) {
        current->child = child;
    } else {
        process_t *sib = current->child;
        while (sib->sibling)
            sib = sib->sibling;
        sib->sibling = child;
    }

    /*
     * Copy the parent's kernel stack contents to the child's stack.
     */
    if (current->kernel_stack) {
        uint64_t parent_stack_top = (uint64_t)current->kernel_stack +
                                     (KERNEL_STACK_PAGES * PAGE_SIZE);

        uint64_t parent_rsp;
        __asm__ volatile ("mov %%rsp, %0" : "=r"(parent_rsp));

        uint64_t parent_used = parent_stack_top - parent_rsp;
        uint64_t child_rsp   = child_stack_top - parent_used;

        memcpy((void *)child_rsp, (void *)parent_rsp, parent_used);

        child->kernel_rsp = child_rsp;
        child->trap_frame = NULL;

        /* 将 trap frame 的 rax 修补为 0，使子进程从 fork 返回 0 */
        if (current->trap_frame) {
            int64_t tf_offset = (int64_t)((uint64_t)current->trap_frame -
                                           parent_stack_top);
            trap_frame_t *child_tf = (trap_frame_t *)(
                (int64_t)child_stack_top + tf_offset);
            child_tf->rax = 0;
            child->trap_frame = child_tf;
        }
    } else {
        /* Parent is PID 0 using the boot stack – can't copy.
         * Build a minimal frame that returns 0. */
        uint64_t *sp = (uint64_t *)child_stack_top;
        *--sp = 0;   /* arg (not used) */
        *--sp = 0;   /* entry (not used) */
        *--sp = (uint64_t)kthread_trampoline_asm;
        *--sp = 0;  /* rbp */
        *--sp = 0;  /* rbx */
        *--sp = 0;  /* r12 */
        *--sp = 0;  /* r13 */
        *--sp = 0;  /* r14 */
        *--sp = 0;  /* r15 */
        child->kernel_rsp = (uint64_t)sp;
        child->trap_frame = NULL;
    }

    return pid;
}

/* ========================================================================
 * process_exit()
 * ======================================================================== */

void process_exit(int code)
{
    if (!current)
        hal_halt();

    current->exit_code = code;

    /* Close all open file descriptors */
    for (int i = 0; i < PROC_MAX_FD; i++) {
        if (current->fd_table[i]) {
            vfs_file_t *file = current->fd_table[i];
            file->refcount--;
            if (file->refcount <= 0)
                free_page(file);
            current->fd_table[i] = NULL;
        }
    }

    /* Stack is freed by the parent in process_wait(), not here,
     * because we're still running on it. */

    /* 将子进程重新挂载到 PID 0（需要锁保护进程树操作） */
    spinlock_acquire(&proc_table_lock);
    current->state = PROC_ZOMBIE;

    /* Wake up parent if it's waiting */
    if (current->parent && current->parent->state == PROC_BLOCKED)
        current->parent->state = PROC_READY;

    process_t *ch = current->child;
    while (ch) {
        ch->ppid = 0;
        ch->parent = &proc_table[0];
        if (!proc_table[0].child) {
            proc_table[0].child = ch;
        } else {
            process_t *sib = proc_table[0].child;
            while (sib->sibling)
                sib = sib->sibling;
            sib->sibling = ch;
        }
        process_t *next_ch = ch->sibling;
        ch->sibling = NULL;
        ch = next_ch;
    }
    spinlock_release(&proc_table_lock);

    this_cpu()->need_reschedule = 1;
    scheduler_schedule();

    /* 不应到达此处 - 调度器会切换到另一个进程 */
    __builtin_unreachable();
}

/* ========================================================================
 * process_wait() with reap_zombie()
 * ======================================================================== */

static int reap_zombie(process_t *child, process_t *prev, int *status)
{
    int child_pid = child->pid;
    if (status)
        *status = (int)child->exit_code;

    /* Remove from parent's child list */
    if (prev)
        prev->sibling = child->sibling;
    else
        current->child = child->sibling;

    /* Free kernel stack */
    if (child->kernel_stack) {
        free_page(child->kernel_stack);
        free_page((void *)((uint64_t)child->kernel_stack + PAGE_SIZE));
    }

    child->state = PROC_UNUSED;
    return child_pid;
}

/* reap_zombie 的加锁版本 */
static int reap_zombie_locked(process_t *child, process_t *prev, int *status)
{
    spinlock_acquire(&proc_table_lock);
    int ret = reap_zombie(child, prev, status);
    spinlock_release(&proc_table_lock);
    return ret;
}

int process_wait(int pid, int *status, int options)
{
    (void)options;

    if (!current)
        return -1;

    /* 扫描僵尸子进程 */
    process_t *child = current->child;
    process_t *prev  = NULL;

    while (child) {
        if ((pid == -1 || child->pid == pid) &&
            child->state == PROC_ZOMBIE) {
            return reap_zombie_locked(child, prev, status);
        }
        prev = child;
        child = child->sibling;
    }

    /* 验证指定的子进程是否存在 */
    if (pid != -1) {
        process_t *target = process_get(pid);
        if (!target || target->ppid != current->pid)
            return -1;
    }

    /* 阻塞直到子进程退出 */
    current->state = PROC_BLOCKED;
    scheduler_schedule();

    /* 被唤醒 – 重新扫描僵尸进程 */
    child = current->child;
    prev  = NULL;
    while (child) {
        if (child->state == PROC_ZOMBIE)
            return reap_zombie_locked(child, prev, status);
        prev = child;
        child = child->sibling;
    }

    return -1;
}

/* ========================================================================
 * process_kill() - 终止进程
 * ======================================================================== */

int process_kill(int pid, int sig)
{
    if (sig < 1 || sig >= MAX_SIGNAL)
        return -1;

    spinlock_acquire(&proc_table_lock);
    process_t *target = process_get(pid);
    if (!target) {
        spinlock_release(&proc_table_lock);
        return -1;
    }

    target->pending_signals |= (1ULL << (sig - 1));

    if (target->state == PROC_BLOCKED)
        target->state = PROC_READY;

    spinlock_release(&proc_table_lock);
    return 0;
}

/* ========================================================================
 * process_clone() – 通过 clone 系统调用创建新线程（或进程）
 *
 * 支持 CLONE_VM（线程共享地址空间）和 CLONE_THREAD。
 * 如果设置了 CLONE_VM，子进程共享父进程的 PML4。
 *
 * 已修复的死代码分支：CLONE_FILES 和 CLONE_SIGHAND 块中
 * 相同的 if/else 体现在已简化为单次代码。
 * ======================================================================== */

int process_clone(uint64_t flags, uint64_t child_stack,
                  uint64_t ptid, uint64_t ctid, uint64_t newtls)
{
    if (!current)
        return -1;

    int pid = alloc_pid();
    if (pid < 0)
        return -1;

    process_t *child = &proc_table[pid];
    memset(child, 0, sizeof(process_t));

    /* Copy process attributes */
    child->pid       = pid;
    child->ppid      = (flags & CLONE_THREAD) ? current->ppid : current->pid;
    child->state     = PROC_READY;
    child->flags     = current->flags;
    child->uid       = current->uid;
    child->euid      = current->euid;
    child->gid       = current->gid;
    child->egid      = current->egid;
    child->priority  = DEFAULT_TIMESLICE;
    child->exit_code = 0;
    child->sleep_until = 0;
    child->cpu_time  = 0;

    /* Address space: share if CLONE_VM, otherwise share for now (no COW) */
    child->pml4         = current->pml4;
    child->entry_point  = current->entry_point;
    child->brk          = current->brk;
    child->stack_top    = child_stack ? child_stack : current->stack_top;
    child->mmap_base    = current->mmap_base;
    child->mmap_current = current->mmap_current;

    /* Allocate a new kernel stack (2 contiguous pages) */
    void *stack = alloc_pages(KERNEL_STACK_PAGES);
    if (!stack) {
        child->state = PROC_UNUSED;
        return -1;
    }
    child->kernel_stack = stack;

    /* File descriptors: always copy with refcount increment
     * (previously had dead if/else with CLONE_FILES – both branches were identical) */
    memcpy(child->fd_table, current->fd_table, sizeof(child->fd_table));
    for (int i = 0; i < PROC_MAX_FD; i++) {
        if (child->fd_table[i])
            child->fd_table[i]->refcount++;
    }
    strcpy(child->cwd, current->cwd);

    /* 信号处理：始终从父进程复制
     *（之前有 CLONE_SIGHAND 的死 if/else – 两个分支完全相同） */
    child->signal_mask     = current->signal_mask;
    child->pending_signals = 0;
    memcpy(child->signal_handlers, current->signal_handlers,
           sizeof(child->signal_handlers));
    memcpy(child->signal_flags, current->signal_flags,
           sizeof(child->signal_flags));
    child->signal_restorer = current->signal_restorer;

    /* SFK */
    child->sfk_perms = current->sfk_perms;
    strcpy(child->sfk_pkg_id, current->sfk_pkg_id);

    /* 进程树 */
    child->parent  = current;
    child->next    = NULL;
    child->child   = NULL;
    child->sibling = NULL;

    if (!current->child) {
        current->child = child;
    } else {
        process_t *sib = current->child;
        while (sib->sibling)
            sib = sib->sibling;
        sib->sibling = child;
    }

    /* 复制内核栈并修补 trap frame，类似于 fork */
    if (current->kernel_stack) {
        uint64_t parent_stack_top = (uint64_t)current->kernel_stack +
                                     (KERNEL_STACK_PAGES * PAGE_SIZE);

        uint64_t parent_rsp;
        __asm__ volatile ("mov %%rsp, %0" : "=r"(parent_rsp));

        uint64_t parent_used = parent_stack_top - parent_rsp;
        uint64_t child_stack_top = (uint64_t)stack +
                                    (KERNEL_STACK_PAGES * PAGE_SIZE);
        uint64_t child_rsp = child_stack_top - parent_used;

        memcpy((void *)child_rsp, (void *)parent_rsp, parent_used);

        child->kernel_rsp = child_rsp;

        /* 修补 trap frame rax=0（子进程从 clone 返回 0） */
        if (current->trap_frame) {
            int64_t tf_offset = (int64_t)((uint64_t)current->trap_frame -
                                           parent_stack_top);
            trap_frame_t *child_tf = (trap_frame_t *)(
                (int64_t)child_stack_top + tf_offset);
            child_tf->rax = 0;

            /* Set child's user stack to child_stack if provided */
            if (child_stack)
                child_tf->rsp = child_stack;

            child->trap_frame = child_tf;
        }
    } else {
        /* 父进程是 PID 0，使用启动栈 */
        uint64_t child_stack_top = (uint64_t)stack +
                                    (KERNEL_STACK_PAGES * PAGE_SIZE);
        uint64_t *sp = (uint64_t *)child_stack_top;
        *--sp = 0;
        *--sp = 0;
        *--sp = (uint64_t)kthread_trampoline_asm;
        *--sp = 0;  /* rbp */
        *--sp = 0;  /* rbx */
        *--sp = 0;  /* r12 */
        *--sp = 0;  /* r13 */
        *--sp = 0;  /* r14 */
        *--sp = 0;  /* r15 */
        child->kernel_rsp = (uint64_t)sp;
        child->trap_frame = NULL;
    }

    /* CLONE_SETTLS：通过 arch_prctl 设置线程本地存储 */
    if ((flags & CLONE_SETTLS) && newtls) {
        /* newtls 参数是 struct user_desc* 或直接的 FS 基地址。
         * 对于 64 位，它直接就是 FS 基地址。 */
        /* 这将在子进程被调度时应用 */
        /* 存储在稍后可以访问的位置 - 为简化起见，
         * 我们在此不实现完整的 TLS 设置 */
    }

    /* CLONE_PARENT_SETTID: store child tid at ptid */
    if ((flags & CLONE_PARENT_SETTID) && ptid) {
        int *ptid_ptr = (int *)(uintptr_t)ptid;
        *ptid_ptr = pid;
    }

    /* CLONE_CHILD_CLEARTID: arrange for ctid to be cleared on child exit */
    if (flags & CLONE_CHILD_CLEARTID) {
        /* Store ctid for later use - simplified: we skip this for now */
    }

    return pid;
}

/* ========================================================================
 * COW page fault handler
 *
 * Called from the page fault exception handler (idt.c) when a write
 * fault occurs on a read-only page that has PTE_COW set.
 *
 * Returns:
 *   0  - COW handled successfully, retry the faulting instruction
 *  -1  - Not a COW fault, caller should treat as a real segfault
 *
 * Refactored to use mmu_walk_page() and mmu_virt_to_phys().
 * ======================================================================== */

int process_cow_page_fault(uint64_t fault_addr)
{
    if (!current)
        return -1;

    if (!(current->flags & PROC_FLAG_COW))
        return -1;

    uint64_t page_vaddr = fault_addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t *pte = mmu_walk_page(current->pml4, page_vaddr, 0);
    if (!pte || !(*pte & PTE_PRESENT))
        return -1;

    /* 检查 COW 标记（位 9） */
    if (!(*pte & PTE_COW))
        return -1;

    uint64_t old_phys = *pte & PTE_ADDR_MASK;
    void *new_page = alloc_page();
    if (!new_page)
        return -1;

    memcpy(new_page, (void *)(uintptr_t)old_phys, PAGE_SIZE);

    uint64_t new_phys = (uint64_t)(uintptr_t)new_page;
    *pte = new_phys | (*pte & ~(PTE_ADDR_MASK | PTE_COW)) | PTE_WRITABLE;

    hal_flush_tlb_page((uintptr_t)page_vaddr);
    return 0;
}
