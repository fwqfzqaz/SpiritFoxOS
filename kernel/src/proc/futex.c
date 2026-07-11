/*
 * SpiritFoxOS Futex Implementation
 *
 * Simplified futex wait/wake using a static wait queue.
 * Extracted from process.c for modularity.
 * Refactored to use mmu_virt_to_phys() instead of manual page table walks.
 */

#include "process.h"
#include "hal.h"
#include "memory.h"
#include "timer.h"
#include "errno.h"
#include "mmu.h"

extern process_t *current;

/* ========================================================================
 * Futex wait queue
 * ======================================================================== */

#define MAX_FUTEX_WAITERS 64

typedef struct {
    int         active;
    uint32_t   *uaddr;
    process_t  *proc;
} futex_waiter_t;

static futex_waiter_t futex_waiters[MAX_FUTEX_WAITERS];

/* ========================================================================
 * process_futex_wait() – wait on a futex
 * ======================================================================== */

int process_futex_wait(uint32_t *uaddr, uint32_t val, uint64_t timeout_ms)
{
    if (!uaddr)
        return -1;

    uint64_t phys = mmu_virt_to_phys(current->pml4, (uint64_t)(uintptr_t)uaddr);
    if (phys == 0)
        return -1;

    uint32_t cur_val = *(volatile uint32_t *)(uintptr_t)phys;

    if (cur_val != val)
        return -1;

    int slot = -1;
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (!futex_waiters[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;

    futex_waiters[slot].active = 1;
    futex_waiters[slot].uaddr  = uaddr;
    futex_waiters[slot].proc   = current;

    if (timeout_ms > 0) {
        process_sleep(timeout_ms);
    } else {
        current->state = PROC_BLOCKED;
        scheduler_schedule();
    }

    futex_waiters[slot].active = 0;

    return 0;
}

/* ========================================================================
 * process_futex_wake() – 唤醒等待 futex 的进程
 * ======================================================================== */

int process_futex_wake(uint32_t *uaddr, int count)
{
    int woken = 0;

    for (int i = 0; i < MAX_FUTEX_WAITERS && woken < count; i++) {
        if (futex_waiters[i].active && futex_waiters[i].uaddr == uaddr) {
            futex_waiters[i].active = 0;
            if (futex_waiters[i].proc->state == PROC_BLOCKED) {
                futex_waiters[i].proc->state = PROC_READY;
                futex_waiters[i].proc->sleep_until = 0;
            }
            woken++;
        }
    }

    return woken;
}
