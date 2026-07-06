/*
 * SpiritFoxOS Scheduler
 *
 * Round-robin process scheduler with context switching.
 * Extracted from process.c for modularity.
 */

#include "process.h"
#include "gdt.h"
#include "hal.h"
#include "memory.h"
#include "timer.h"
#include "string.h"
#include "serial.h"
#include "vga.h"

/* Process table - defined in process.c */
extern process_t proc_table[];
extern process_t *current;
extern int need_reschedule;

/* Context switch primitive - defined in isr_stub.S */
extern void arch_switch_to(uint64_t *old_rsp_ptr, uint64_t new_rsp);
#define switch_to(old_rsp_ptr, new_rsp) arch_switch_to(old_rsp_ptr, new_rsp)

/* Assembly trampoline - defined in isr_stub.S */
extern void kthread_trampoline_asm(void);

/* ========================================================================
 * Constants
 * ======================================================================== */

#define MAX_PROCS          256
#define DEFAULT_TIMESLICE  20      /* 20ms at 1000Hz */
#define KERNEL_STACK_PAGES 2       /* 2 pages = 8KB kernel stack */

/* ========================================================================
 * PID allocation
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
 * Process lookup
 * ======================================================================== */

process_t *process_get(int pid)
{
    if (pid < 0 || pid >= MAX_PROCS)
        return NULL;
    if (proc_table[pid].state == PROC_UNUSED)
        return NULL;
    return &proc_table[pid];
}

process_t *process_current(void)
{
    return current;
}

/* ========================================================================
 * Reschedule flag
 * ======================================================================== */

int need_reschedule_check(void)
{
    if (need_reschedule) {
        need_reschedule = 0;
        return 1;
    }
    return 0;
}

/* ========================================================================
 * process_yield()
 * ======================================================================== */

void process_yield(void)
{
    if (current)
        current->state = PROC_READY;
    need_reschedule = 1;
    scheduler_schedule();
}

/* ========================================================================
 * process_sleep()
 * ======================================================================== */

void process_sleep(uint64_t ms)
{
    if (!current)
        return;
    current->sleep_until = timer_get_ms() + ms;
    current->state = PROC_BLOCKED;
    scheduler_schedule();
}

/* ========================================================================
 * Scheduler tick (called from timer interrupt)
 * ======================================================================== */

void scheduler_tick(void)
{
    if (!current)
        return;

    current->cpu_time++;
    current->priority--;

    /* Wake sleeping processes whose time has come */
    uint64_t now = timer_get_ms();
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_BLOCKED &&
            proc_table[i].sleep_until > 0 &&
            proc_table[i].sleep_until <= now) {
            proc_table[i].state = PROC_READY;
            proc_table[i].sleep_until = 0;
        }
    }

    if (current->priority <= 0)
        need_reschedule = 1;
}

/* ========================================================================
 * Scheduler - round-robin context switch
 * ======================================================================== */

__attribute__((noinline))
void scheduler_schedule(void)
{
    if (!current)
        return;

    /* Round-robin: find the next READY process after current */
    int start = current->pid;
    int next = -1;

    for (int i = 1; i <= MAX_PROCS; i++) {
        int idx = (start + i) % MAX_PROCS;
        if (proc_table[idx].state == PROC_READY) {
            next = idx;
            break;
        }
    }

    /* No other READY process */
    if (next < 0) {
        if (current->state == PROC_RUNNING ||
            current->state == PROC_READY) {
            current->state = PROC_RUNNING;
            current->priority = DEFAULT_TIMESLICE;
            return;
        }
        /* Current is not runnable – find anyone */
        for (int i = 0; i < MAX_PROCS; i++) {
            if (proc_table[i].state == PROC_READY) {
                next = i;
                break;
            }
        }
        if (next < 0) {
            /* Fall back to idle process */
            if (proc_table[0].state != PROC_UNUSED) {
                next = 0;
                proc_table[0].state = PROC_READY;
            } else {
                hal_halt();
            }
        }
    }

    process_t *old   = current;
    process_t *new_p = &proc_table[next];

    /* Same process – no switch needed */
    if (old == new_p) {
        old->state = PROC_RUNNING;
        old->priority = DEFAULT_TIMESLICE;
        need_reschedule = 0;
        return;
    }

    if (old->state == PROC_RUNNING)
        old->state = PROC_READY;

    new_p->state = PROC_RUNNING;
    new_p->priority = DEFAULT_TIMESLICE;
    need_reschedule = 0;

    /* Switch page tables if different */
    if (old->pml4 != new_p->pml4)
        hal_write_cr3(new_p->pml4);

    /* Update TSS rsp0 for interrupt privilege transitions */
    if (new_p->kernel_stack) {
        tss.rsp0 = (uint64_t)new_p->kernel_stack +
                    (KERNEL_STACK_PAGES * PAGE_SIZE);
    } else {
        /* PID 0 uses the boot stack; approximate rsp0 */
        uint64_t rsp_approx;
        __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp_approx));
        tss.rsp0 = rsp_approx + 512;
    }

    /* Debug output BEFORE the context switch */
    printf("[SCHED] PID%d -> PID%d krsp=%lx stack[0]=%lx stack[1]=%lx stack[2]=%lx stack[3]=%lx\n",
           old->pid, new_p->pid,
           (unsigned long)new_p->kernel_rsp,
           (unsigned long)((uint64_t *)new_p->kernel_rsp)[0],
           (unsigned long)((uint64_t *)new_p->kernel_rsp)[1],
           (unsigned long)((uint64_t *)new_p->kernel_rsp)[2],
           (unsigned long)((uint64_t *)new_p->kernel_rsp)[3]);

    /* Context switch */
    current = new_p;
    switch_to(&old->kernel_rsp, new_p->kernel_rsp);

    /* Execution reaches here when THIS process is scheduled back in. */
}

/* ========================================================================
 * scheduler_start() - enter the idle loop (never returns)
 * ======================================================================== */

void scheduler_start(void)
{
    hal_enable_interrupts();
    while (1)
        hal_halt_no_cli();
    __builtin_unreachable();
}
