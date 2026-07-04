/*
 * SpiritFoxOS Process Management
 *
 * Core process creation, lifecycle, and COW support.
 * Scheduler, signals, futex, and user-mode code are in separate files:
 *   scheduler.c, signal.c, futex.c, process_user.c
 */

#include "process.h"
#include "gdt.h"
#include "hal.h"
#include "memory.h"
#include "kmalloc.h"
#include "timer.h"
#include "string.h"
#include "vfs.h"
#include "elf64.h"
#include "vga.h"
#include "serial.h"
#include "mmu.h"
#include "clone.h"

/* ========================================================================
 * Constants
 * ======================================================================== */

#define PTE_COW (1ULL << 9)  /* Custom bit for COW tracking */

#define MAX_PROCS          256
#define DEFAULT_TIMESLICE  20      /* 20ms at 1000Hz */
#define KERNEL_STACK_PAGES 2       /* 2 pages = 8KB kernel stack */

/* ========================================================================
 * Process table (non-static: shared with scheduler.c, signal.c, etc.)
 * ======================================================================== */

process_t proc_table[MAX_PROCS];
process_t *current = NULL;
int need_reschedule = 0;

/* ========================================================================
 * Context switch primitive - in isr_stub.S
 * ======================================================================== */

extern void arch_switch_to(uint64_t *old_rsp_ptr, uint64_t new_rsp);

/* Assembly trampoline - in isr_stub.S */
extern void kthread_trampoline_asm(void);

/* ========================================================================
 * PID allocation (used only in this file)
 * ======================================================================== */

static int alloc_pid(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED)
            return i;
    }
    return -1;
}

/* ========================================================================
 * Kernel thread trampoline
 * ======================================================================== */

/* Called from the trampoline with (entry, arg) in rdi, rsi */
__attribute__((used)) void kthread_entry(void (*entry)(void *), void *arg)
{
    printf("[kthread_entry] entry=%p arg=%p\n", (void *)entry, arg);
    entry(arg);
    process_exit(0);
    __builtin_unreachable();
}

/* ========================================================================
 * process_init()
 * ======================================================================== */

void process_init(void)
{
    memset(proc_table, 0, sizeof(proc_table));

    /* Initialize the TSS */
    memset(&tss, 0, sizeof(tss));
    tss.iomap_base = sizeof(tss_t);

    /* Create PID 0 – the idle/kernel process */
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

    /* Memory – share the kernel page table */
    p->pml4         = hal_read_cr3();
    p->kernel_stack = NULL;       /* PID 0 uses the boot stack */
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

    /* Process tree */
    p->parent  = NULL;
    p->next    = NULL;
    p->child   = NULL;
    p->sibling = NULL;

    /* Saved context – kernel_rsp will be set on first context switch out */
    p->trap_frame  = NULL;
    p->kernel_rsp  = 0;

    current = p;
    tss.rsp0 = 0;
}

/* ========================================================================
 * process_create_kthread()
 * ======================================================================== */

process_t *process_create_kthread(void (*entry)(void *), void *arg)
{
    int pid = alloc_pid();
    if (pid < 0)
        return NULL;

    process_t *p = &proc_table[pid];
    memset(p, 0, sizeof(process_t));

    /* Allocate kernel stack (2 contiguous pages) */
    void *stack = alloc_pages(KERNEL_STACK_PAGES);
    if (!stack)
        return NULL;

    uint64_t stack_top = (uint64_t)stack + (KERNEL_STACK_PAGES * PAGE_SIZE);

    /* Fill in process attributes */
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

    /* Link into parent's child list */
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

    /* Build initial stack frame for arch_switch_to */
    uint64_t *sp = (uint64_t *)stack_top;
    *--sp = (uint64_t)arg;                   /* for rsi via trampoline */
    *--sp = (uint64_t)entry;                 /* for rdi via trampoline */
    *--sp = (uint64_t)kthread_trampoline_asm;    /* return address */
    *--sp = 0;  /* rbp */
    *--sp = 0;  /* rbx */
    *--sp = 0;  /* r12 */
    *--sp = 0;  /* r13 */
    *--sp = 0;  /* r14 */
    *--sp = 0;  /* r15 */

    p->kernel_rsp = (uint64_t)sp;
    p->trap_frame = NULL;

    printf("[kthread_create] PID=%d stack=%p stack_top=%lx krsp=%lx entry=%p arg=%p\n",
           p->pid, stack, (unsigned long)stack_top, (unsigned long)p->kernel_rsp,
           (void *)entry, arg);
    printf("[kthread_create] stack contents: r15=%lx r14=%lx r13=%lx r12=%lx rbx=%lx rbp=%lx ret=%lx entry=%lx arg=%lx\n",
           (unsigned long)sp[0], (unsigned long)sp[1], (unsigned long)sp[2],
           (unsigned long)sp[3], (unsigned long)sp[4], (unsigned long)sp[5],
           (unsigned long)sp[6], (unsigned long)sp[7], (unsigned long)sp[8]);

    /* Verify: volatile read-back from kernel_rsp */
    {
        volatile uint64_t *vp = (volatile uint64_t *)p->kernel_rsp;
        printf("[kthread_create] verify: v[0]=%lx v[6]=%lx v[7]=%lx v[8]=%lx\n",
               (unsigned long)vp[0], (unsigned long)vp[6],
               (unsigned long)vp[7], (unsigned long)vp[8]);
    }

    return p;
}

/* ========================================================================
 * process_fork()
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

    /* ---- COW: Create new PML4, share user pages as read-only ---- */
    {
        /* 1. Allocate new PML4 */
        void *new_pml4_page = alloc_page();
        if (!new_pml4_page) {
            child->state = PROC_UNUSED;
            return -1;
        }
        memset(new_pml4_page, 0, PAGE_SIZE);
        uint64_t *dst_pml4 = (uint64_t *)new_pml4_page;
        uint64_t *src_pml4 = (uint64_t *)(uintptr_t)current->pml4;

        /* 2. Copy kernel half (entries 256-511) */
        for (int i = 256; i < 512; i++)
            dst_pml4[i] = src_pml4[i];

        /* 3. Copy user half (entries 0-255) - share page tables
         *    and mark all user pages as read-only for COW */
        for (int i = 0; i < 256; i++) {
            if (src_pml4[i] & PTE_PRESENT) {
                /* Share the PDPT page by mapping it in both */
                dst_pml4[i] = src_pml4[i];

                /* Walk PDPT -> PD -> PT and clear PTE_WRITABLE,
                 * set PTE_COW */
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

        /* Mark both parent and child as COW processes */
        child->flags  |= PROC_FLAG_COW;
        current->flags |= PROC_FLAG_COW;
    }

    /* Allocate a new kernel stack for the child */
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

    /* Process tree */
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

        /* Patch trap frame rax to 0 so the child returns 0 from fork */
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

    current->state = PROC_ZOMBIE;

    /* Wake up parent if it's waiting */
    if (current->parent && current->parent->state == PROC_BLOCKED)
        current->parent->state = PROC_READY;

    /* Reparent children to PID 0 */
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

    need_reschedule = 1;
    scheduler_schedule();

    /* Should never reach here - scheduler switches to another process */
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

int process_wait(int pid, int *status, int options)
{
    (void)options;

    if (!current)
        return -1;

    /* Scan for zombie children */
    process_t *child = current->child;
    process_t *prev  = NULL;

    while (child) {
        if ((pid == -1 || child->pid == pid) &&
            child->state == PROC_ZOMBIE) {
            return reap_zombie(child, prev, status);
        }
        prev = child;
        child = child->sibling;
    }

    /* Validate that the specific child exists */
    if (pid != -1) {
        process_t *target = process_get(pid);
        if (!target || target->ppid != current->pid)
            return -1;
    }

    /* Block until a child exits */
    current->state = PROC_BLOCKED;
    scheduler_schedule();

    /* Woke up – rescan for zombies */
    child = current->child;
    prev  = NULL;
    while (child) {
        if (child->state == PROC_ZOMBIE)
            return reap_zombie(child, prev, status);
        prev = child;
        child = child->sibling;
    }

    return -1;
}

/* ========================================================================
 * process_kill()
 * ======================================================================== */

int process_kill(int pid, int sig)
{
    if (sig < 1 || sig >= MAX_SIGNAL)
        return -1;

    process_t *target = process_get(pid);
    if (!target)
        return -1;

    target->pending_signals |= (1ULL << (sig - 1));

    if (target->state == PROC_BLOCKED)
        target->state = PROC_READY;

    return 0;
}

/* ========================================================================
 * process_clone() – create a new thread (or process) via clone syscall
 *
 * Supports CLONE_VM (shared address space for threads) and CLONE_THREAD.
 * If CLONE_VM is set, the child shares the parent's PML4.
 *
 * Dead code branches fixed: CLONE_FILES and CLONE_SIGHAND blocks that
 * had identical if/else bodies are now simplified to just the code once.
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

    /* Signal handling: always copy from parent
     * (previously had dead if/else with CLONE_SIGHAND – both branches were identical) */
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

    /* Process tree */
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

    /* Copy kernel stack and patch trap frame, similar to fork */
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

        /* Patch trap frame rax=0 (child returns 0 from clone) */
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
        /* Parent is PID 0 using boot stack */
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

    /* CLONE_SETTLS: set thread-local storage via arch_prctl */
    if ((flags & CLONE_SETTLS) && newtls) {
        /* The newtls parameter is the struct user_desc* or direct FS base.
         * For 64-bit, it's the FS base address directly. */
        /* This will be applied when the child is scheduled in */
        /* Store in a place we can access later - for simplicity,
         * we don't implement full TLS setup here */
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

    /* Check COW marker (bit 9) */
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
