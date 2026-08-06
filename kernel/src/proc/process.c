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
 * fork 子进程返回函数
 *
 * 当 fork 创建的子进程首次被调度时，通过 kthread_trampoline 进入此函数。
 * arg 是子进程的 process_t 指针，其中 trap_frame 包含了 fork 时的用户态上下文
 * （rax 已被设为 0）。
 * 此函数设置好内核状态后，通过 process_enter_user() 跳转到用户态，
 * 使子进程从 fork 返回点继续执行，返回值为 0。
 * ======================================================================== */
static void fork_child_return(void *arg)
{
    process_t *self = (process_t *)arg;

    /* 设置 current 指针（调度器已设置 cpu->current_process，
     * 但全局 current 可能未同步） */
    current = self;

    /* 恢复 FS_BASE（TLS 指针）。
     * musl libc 通过 FS 段寄存器访问 errno 等线程局部变量。
     * fork 后子进程必须恢复与父进程相同的 FS_BASE 值，
     * 否则所有 TLS 访问都会读取地址 0（崩溃）。 */
    if (self->fs_base)
        hal_write_msr(0xC0000100U, self->fs_base);  /* MSR_IA32_FS_BASE */

    /* 设置 TSS rsp0 和 syscall 暂存字段 */
    if (self->kernel_stack) {
        gdt_set_tss_rsp0(this_cpu()->index,
            (uint64_t)self->kernel_stack + (KERNEL_STACK_PAGES * PAGE_SIZE));
    }

    cpu_local_t *cpu = this_cpu();
    cpu->kernel_rsp_top = (self->kernel_stack)
        ? (uint64_t)self->kernel_stack + (KERNEL_STACK_PAGES * PAGE_SIZE)
        : cpu_gdts[cpu->index].tss.rsp0;
    cpu->process_pml4 = self->pml4;

    /* 确保 swapgs 状态正确：我们是从内核态通过调度器进入的，
     * 此时 GS_BASE = cpu_locals（内核态），KERNEL_GS_BASE = 0。
     * process_enter_user() 中的 swapgs 会交换到 GS_BASE=0（用户态），
     * 这是正确的。 */

    serial_puts("[fork_child] PID=");
    serial_put_dec((uint64_t)self->pid);
    serial_puts(" returning to user rip=0x");
    if (self->trap_frame)
        serial_put_hex(self->trap_frame->rip);
    serial_puts("\n");

    /* 通过 process_enter_user() 跳转到用户态 — 不返回 */
    if (self->trap_frame)
        process_enter_user(self->trap_frame);

    /* 不应到达这里 */
    serial_puts("[fork_child] ERROR: process_enter_user returned!\n");
    process_exit(1);
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
    serial_puts("[kthread] alloc_pid...\n");
    int pid = alloc_pid();
    if (pid < 0) {
        serial_puts("[kthread] alloc_pid failed\n");
        return NULL;
    }
    serial_puts("[kthread] pid=");
    serial_put_dec((uint64_t)pid);
    serial_puts(" alloc_stack...\n");

    process_t *p = &proc_table[pid];
    memset(p, 0, sizeof(process_t));

    /* 分配内核栈（2 个连续页） */
    void *stack = alloc_pages(KERNEL_STACK_PAGES);
    if (!stack) {
        serial_puts("[kthread] alloc_pages failed\n");
        return NULL;
    }
    serial_puts("[kthread] stack=");
    serial_put_hex((uint64_t)(uintptr_t)stack);
    serial_puts("\n");

    uint64_t stack_top = (uint64_t)stack + (KERNEL_STACK_PAGES * PAGE_SIZE);

    /* 填充进程属性 */
    p->pid           = pid;
    p->ppid          = current ? current->pid : 0;
    p->state         = PROC_UNUSED;  /* 防止调度器在初始化完成前扫描到 */
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

    /* 初始化完成，标记为 READY 并入队 */
    p->state = PROC_READY;
    process_enqueue(p);

    return p;
}

/* ========================================================================
 * process_fork() - 进程分叉
 * ======================================================================== */

int process_fork(trap_frame_t *caller_frame)
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

    /* ---- COW：创建独立页表副本，将用户页共享为只读 ----
     *
     * 关键设计：子进程拥有独立的页表结构（PML4/PDPT/PD/PT），
     * 但叶子页（4KB 数据页）是共享的（COW）。
     * 这样当 COW 缺页处理时，只更新当前进程的 PTE，
     * 不会影响另一个进程的页表。
     */
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

        /* 2. 复制内核半区（项 256-511）- 共享内核映射 */
        for (int i = 256; i < 512; i++)
            dst_pml4[i] = src_pml4[i];

        /* 3. 递归复制用户半区页表，叶子页标记 COW */
        for (int i = 0; i < 256; i++) {
            if (!(src_pml4[i] & PTE_PRESENT))
                continue;

            /* 分配新的 PDPT 页 */
            void *new_pdpt_page = alloc_page();
            if (!new_pdpt_page) {
                /* TODO: 清理已分配的页 */
                child->state = PROC_UNUSED;
                return -1;
            }
            uint64_t *dst_pdpt = (uint64_t *)new_pdpt_page;
            uint64_t *src_pdpt = (uint64_t *)(src_pml4[i] & PTE_ADDR_MASK);
            uint64_t pdpt_flags = src_pml4[i] & ~PTE_ADDR_MASK;

            for (int j = 0; j < 512; j++) {
                if (!(src_pdpt[j] & PTE_PRESENT))
                    continue;
                if (src_pdpt[j] & PTE_HUGE) {
                    /* 1GB 大页：直接共享 */
                    dst_pdpt[j] = src_pdpt[j];
                    continue;
                }

                /* 分配新的 PD 页 */
                void *new_pd_page = alloc_page();
                if (!new_pd_page) {
                    child->state = PROC_UNUSED;
                    return -1;
                }
                uint64_t *dst_pd = (uint64_t *)new_pd_page;
                uint64_t *src_pd = (uint64_t *)(src_pdpt[j] & PTE_ADDR_MASK);
                uint64_t pd_flags = src_pdpt[j] & ~PTE_ADDR_MASK;

                for (int k = 0; k < 512; k++) {
                    if (!(src_pd[k] & PTE_PRESENT))
                        continue;
                    if (src_pd[k] & PTE_HUGE) {
                        /* 2MB 大页：直接共享 */
                        dst_pd[k] = src_pd[k];
                        continue;
                    }

                    /* 分配新的 PT 页 */
                    void *new_pt_page = alloc_page();
                    if (!new_pt_page) {
                        child->state = PROC_UNUSED;
                        return -1;
                    }
                    uint64_t *dst_pt = (uint64_t *)new_pt_page;
                    uint64_t *src_pt = (uint64_t *)(src_pd[k] & PTE_ADDR_MASK);
                    uint64_t pt_flags = src_pd[k] & ~PTE_ADDR_MASK;

                    for (int l = 0; l < 512; l++) {
                        if (!(src_pt[l] & PTE_PRESENT)) {
                            dst_pt[l] = 0;
                            continue;
                        }
                        /* SHM 页面（PTE_SHM）：保持共享，不 COW。
                         * SHM 包括 memfd_create 创建的共享内存和
                         * DRM dumb buffer 映射。这些页面在父子
                         * 进程间必须共享同一物理页，否则写入
                         * 新分配的 COW 页面不会反映到 DRM
                         * 帧缓冲区，导致画面不更新甚至冻结。 */
                        if (src_pt[l] & PTE_SHM) {
                            dst_pt[l] = src_pt[l];
                            continue;
                        }
                        /* 叶子页：共享物理页，标记只读 + COW */
                        dst_pt[l] = (src_pt[l] & ~PTE_WRITABLE) | PTE_COW;
                        /* 同时将父进程的 PTE 也标记为只读 + COW */
                        src_pt[l] = (src_pt[l] & ~PTE_WRITABLE) | PTE_COW;
                    }

                    dst_pd[k] = (uint64_t)(uintptr_t)new_pt_page | pt_flags;
                }

                dst_pdpt[j] = (uint64_t)(uintptr_t)new_pd_page | pd_flags;
            }

            dst_pml4[i] = (uint64_t)(uintptr_t)new_pdpt_page | pdpt_flags;
        }

        child->pml4 = (uint64_t)(uintptr_t)new_pml4_page;

        /* 刷新父进程 TLB：父进程的 PTE 已被修改为只读，
         * 必须刷新 TLB 以确保后续写入触发 COW 缺页 */
        hal_flush_tlb();

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

    /* 保存 FS_BASE（TLS 指针）用于子进程恢复。
     * musl libc 通过 FS 段寄存器访问 errno 等线程局部变量，
     * 如果 FS_BASE=0 则所有 TLS 访问都会崩溃。 */
    child->fs_base = hal_read_msr(0xC0000100U);  /* MSR_IA32_FS_BASE */

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
     * 设置子进程的启动方式。
     *
     * 子进程使用 kthread_trampoline 启动，入口为 fork_child_return()。
     * 子进程的 trap_frame 通过 kmalloc 分配，保存了 fork 时的用户态上下文
     * （rax=0），来自 caller_frame（syscall 入口压入内核栈的帧）。
     * fork_child_return() 通过 process_enter_user() 跳转回用户态。
     */
    {
        /* 为子进程分配 trap_frame，保存 fork 时的用户态上下文 */
        if (caller_frame) {
            trap_frame_t *child_tf = kmalloc(sizeof(trap_frame_t));
            if (!child_tf) {
                child->state = PROC_UNUSED;
                return -1;
            }
            memcpy(child_tf, caller_frame, sizeof(trap_frame_t));
            child_tf->rax = 0;  /* fork 在子进程返回 0 */
            child->trap_frame = child_tf;
        }

        /* 构建子进程内核栈：使用 kthread_trampoline 格式
         * 栈布局（从高到低）：
         *   arg (child)       ← pop rsi
         *   fork_child_return ← pop rdi  (entry)
         *   kthread_trampoline_asm ← ret address for arch_switch_to
         *   rbp, rbx, r12-r15 ← callee-saved
         */
        uint64_t *sp = (uint64_t *)child_stack_top;
        *--sp = (uint64_t)child;                     /* arg → rsi */
        *--sp = (uint64_t)fork_child_return;         /* entry → rdi */
        *--sp = (uint64_t)kthread_trampoline_asm;    /* return address for arch_switch_to */
        *--sp = 0;  /* rbp */
        *--sp = 0;  /* rbx */
        *--sp = 0;  /* r12 */
        *--sp = 0;  /* r13 */
        *--sp = 0;  /* r14 */
        *--sp = 0;  /* r15 */
        child->kernel_rsp = (uint64_t)sp;
    }

    /* 子进程必须入队，否则调度器无法找到它 */
    process_enqueue(child);

    return pid;
}

/* ========================================================================
 * process_exit()
 * ======================================================================== */

void process_exit(int code)
{
    if (!current)
        hal_halt();

    /* 验证进程表完整性 */
    {
        uintptr_t child_addr = (uintptr_t)current->child;
        uintptr_t table_start = (uintptr_t)&proc_table[0];
        uintptr_t table_end = (uintptr_t)&proc_table[MAX_PROCS];
        if (current->child != NULL &&
            (child_addr < table_start || child_addr >= table_end)) {
            serial_puts("[exit] BUG: child=");
            serial_put_hex((uint64_t)child_addr);
            serial_puts(" out of range!\n");
        }
    }

    current->exit_code = code;

    /* Close all open file descriptors */
    for (int i = 0; i < PROC_MAX_FD; i++) {
        if (current->fd_table[i]) {
            vfs_close(i);
        }
    }

    /* Stack is freed by the parent in process_wait(), not here,
     * because we're still running on it. */

    /* 将子进程重新挂载到 PID 0（需要锁保护进程树操作） */
    spinlock_acquire(&proc_table_lock);
    current->state = PROC_ZOMBIE;

    /* Wake up parent if it's waiting */
    if (current->parent && current->parent->state == PROC_BLOCKED)
        process_wake(current->parent);

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

    /* 切换到其他进程。对于 ZOMBIE 进程，调度器必须永久切换走。
     * 使用循环确保即使调度器意外返回，也不会继续执行已退出的进程。 */
    this_cpu()->need_reschedule = 1;
    for (;;) {
        scheduler_schedule();
        /* 不应到达此处 - 如果到达说明调度器没有切换走 ZOMBIE 进程 */
        serial_puts("[exit] BUG: scheduler returned for ZOMBIE PID=");
        serial_put_dec((uint64_t)current->pid);
        serial_puts("\n");
        /* 强制再试 */
        this_cpu()->need_reschedule = 1;
    }
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
    if (!current)
        return -1;

    serial_puts("[wait] PID=");
    serial_put_dec((uint64_t)current->pid);
    serial_puts(" waitfor=");
    serial_put_dec((uint64_t)pid);
    serial_puts(" options=");
    serial_put_dec((uint64_t)options);
    serial_puts(" child=");
    serial_put_hex((uint64_t)(uintptr_t)current->child);
    serial_puts("\n");

    /* WNOHANG: 如果没有僵尸子进程，立即返回 0 */
    #define WNOHANG 1

    /* 扫描僵尸子进程 */
    process_t *child = current->child;
    process_t *prev  = NULL;

    while (child) {
        /* 验证 child 指针在 proc_table 范围内 */
        uintptr_t child_addr = (uintptr_t)child;
        uintptr_t table_start = (uintptr_t)&proc_table[0];
        uintptr_t table_end = (uintptr_t)&proc_table[MAX_PROCS];
        if (child_addr < table_start || child_addr >= table_end ||
            (child_addr - table_start) % sizeof(process_t) != 0) {
            serial_puts("[wait] BUG: corrupted child pointer=");
            serial_put_hex((uint64_t)child_addr);
            serial_puts(" on PID=");
            serial_put_dec((uint64_t)current->pid);
            serial_puts("\n");
            /* 跳过损坏的指针，清空 child 链表 */
            if (prev)
                prev->sibling = NULL;
            else
                current->child = NULL;
            break;
        }

        if ((pid == -1 || child->pid == pid) &&
            child->state == PROC_ZOMBIE) {
            return reap_zombie_locked(child, prev, status);
        }
        prev = child;
        child = child->sibling;
    }

    /* WNOHANG: 不阻塞，直接返回 0 */
    if (options & WNOHANG)
        return 0;

    /* 验证指定的子进程是否存在 */
    if (pid != -1) {
        process_t *target = process_get(pid);
        if (!target || target->ppid != current->pid)
            return -1;
    }

    /* 阻塞直到子进程退出 */
    serial_puts("[wait] blocking PID=");
    serial_put_dec((uint64_t)current->pid);
    serial_puts("\n");
    current->state = PROC_BLOCKED;
    scheduler_schedule();

    serial_puts("[wait] PID=");
    serial_put_dec((uint64_t)current->pid);
    serial_puts(" woke up, rescanning child=");
    serial_put_hex((uint64_t)(uintptr_t)current->child);
    serial_puts("\n");
    /* 被唤醒 – 重新扫描僵尸进程 */
    child = current->child;
    prev  = NULL;
    while (child) {
        /* 再次验证 child 指针 */
        uintptr_t ca = (uintptr_t)child;
        if (ca < (uintptr_t)&proc_table[0] || ca >= (uintptr_t)&proc_table[MAX_PROCS]) {
            if (prev)
                prev->sibling = NULL;
            else
                current->child = NULL;
            break;
        }
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
        process_wake(target);

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

    /* 子进程必须入队，否则调度器无法找到它 */
    process_enqueue(child);

    /* vfork 语义：如果设置了 CLONE_VM（vfork 场景），
     * 父进程必须阻塞直到子进程调用 execve 或 _exit。
     * 这是因为子进程共享父进程的地址空间，
     * 两者不能同时运行。 */
    if (flags & CLONE_VM) {
        serial_puts("[vfork] parent PID=");
        serial_put_dec((uint64_t)current->pid);
        serial_puts(" blocked, child PID=");
        serial_put_dec((uint64_t)pid);
        serial_puts("\n");
        current->state = PROC_BLOCKED;
        current->sleep_until = 0;  /* 无超时，等待显式唤醒 */
        this_cpu()->need_reschedule = 1;
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

    if (!(current->flags & PROC_FLAG_COW)) {
        static int cow_skip_count = 0;
        cow_skip_count++;
        if (cow_skip_count <= 5) {
            serial_puts("[COW] SKIP: PID=");
            serial_put_dec((uint64_t)current->pid);
            serial_puts(" flags=0x");
            serial_put_hex((uint64_t)current->flags);
            serial_puts(" addr=0x");
            serial_put_hex(fault_addr);
            serial_puts("\n");
        }
        return -1;
    }

    uint64_t page_vaddr = fault_addr & ~(uint64_t)(PAGE_SIZE - 1);

    /* 检测同一地址的重复 COW 错误，防止无限循环 */
    {
        static uint64_t last_cow_addr = 0;
        static int cow_repeat_count = 0;
        if (page_vaddr == last_cow_addr) {
            cow_repeat_count++;
            if (cow_repeat_count > 3) {
                serial_puts("[COW] LOOP DETECTED! addr=0x");
                serial_put_hex(fault_addr);
                serial_puts(" PID=");
                serial_put_dec((uint64_t)current->pid);
                serial_puts(" pml4=0x");
                serial_put_hex(current->pml4);
                serial_puts("\n");
                /* 全局 TLB 刷新作为最后手段 */
                hal_flush_tlb();
                cow_repeat_count = 0;
            }
        } else {
            last_cow_addr = page_vaddr;
            cow_repeat_count = 0;
        }
    }

    uint64_t *pte = mmu_walk_page(current->pml4, page_vaddr, 0);
    if (!pte || !(*pte & PTE_PRESENT)) {
        serial_puts("[COW] FAIL: no PTE for addr=0x");
        serial_put_hex(fault_addr);
        serial_puts(" PID=");
        serial_put_dec((uint64_t)current->pid);
        serial_puts("\n");
        return -1;
    }

    /* 检查 COW 标记（位 9） */
    if (!(*pte & PTE_COW)) {
        serial_puts("[COW] FAIL: not COW pte=0x");
        serial_put_hex(*pte);
        serial_puts(" addr=0x");
        serial_put_hex(fault_addr);
        serial_puts("\n");
        return -1;
    }

    uint64_t old_phys = *pte & PTE_ADDR_MASK;
    void *new_page = alloc_page();
    if (!new_page)
        return -1;

    memcpy(new_page, (void *)(uintptr_t)old_phys, PAGE_SIZE);

    uint64_t new_phys = (uint64_t)(uintptr_t)new_page;
    *pte = new_phys | (*pte & ~(PTE_ADDR_MASK | PTE_COW)) | PTE_WRITABLE;

    /* 刷新 TLB：先尝试单页刷新，如果重复则全局刷新 */
    hal_flush_tlb_page((uintptr_t)page_vaddr);

    /* 验证 PTE 已正确更新 */
    uint64_t *verify_pte = mmu_walk_page(current->pml4, page_vaddr, 0);
    if (verify_pte && !(*verify_pte & PTE_WRITABLE)) {
        serial_puts("[COW] BUG: PTE still not writable after update! pte=0x");
        serial_put_hex(*verify_pte);
        serial_puts("\n");
        hal_flush_tlb();
    }

    {
        static int cow_count = 0;
        cow_count++;
        if (cow_count <= 10 || cow_count % 200 == 0) {
            serial_puts("[COW] #");
            serial_put_dec((uint64_t)cow_count);
            serial_puts(" PID=");
            serial_put_dec((uint64_t)current->pid);
            serial_puts("\n");
        }
    }
    return 0;
}
