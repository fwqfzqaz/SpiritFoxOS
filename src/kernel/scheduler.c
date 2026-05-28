#include "scheduler.h"
#include "pmm.h"
#include "vmm.h"
#include "vga.h"
#include "gdt.h"
#include "../include/stddef.h"
#include <stdint.h>

static process_t processes[MAX_PROCESSES];
static process_t *current_proc = NULL;
static process_t *ready_head = NULL;
static process_t *ready_tail = NULL;
static uint64_t next_pid = 1;

/* Context switch assembly (defined in context.asm) */
extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);

static void enqueue(process_t *proc) {
    proc->next = NULL;
    if (ready_tail) {
        ready_tail->next = proc;
    } else {
        ready_head = proc;
    }
    ready_tail = proc;
}

static process_t *dequeue(void) {
    if (!ready_head) return NULL;
    process_t *proc = ready_head;
    ready_head = proc->next;
    if (!ready_head) ready_tail = NULL;
    proc->next = NULL;
    return proc;
}

void scheduler_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        processes[i].state = PROC_UNUSED;
        processes[i].pid = 0;
        processes[i].next = NULL;
    }
    current_proc = NULL;
    ready_head = NULL;
    ready_tail = NULL;
    next_pid = 1;
}

uint64_t scheduler_create_process(void (*entry)(void)) {
    process_t *proc = NULL;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROC_UNUSED) {
            proc = &processes[i];
            break;
        }
    }
    if (!proc) return (uint64_t)-1;

    proc->pid = next_pid++;
    proc->state = PROC_READY;
    proc->remaining_ticks = TIME_SLICE;

    /* Allocate kernel stack */
    uint64_t stack_phys = pmm_alloc_pages(STACK_SIZE / PAGE_SIZE);
    if (!stack_phys) {
        proc->state = PROC_UNUSED;
        return (uint64_t)-1;
    }
    uint64_t stack_virt = phys_to_virt(stack_phys);
    proc->stack_top = stack_virt + STACK_SIZE;

    /* Set up initial stack frame for context_switch restore */
    uint64_t *sp = (uint64_t *)proc->stack_top;

    /* Push initial register state (callee-saved registers) */
    *--sp = (uint64_t)entry;   /* Return address -> process entry */
    *--sp = 0; /* rbp */
    *--sp = 0; /* rsi */
    *--sp = 0; /* rdi */
    *--sp = 0; /* rbx */
    *--sp = 0; /* r12 */
    *--sp = 0; /* r13 */
    *--sp = 0; /* r14 */
    *--sp = 0; /* r15 */

    proc->rsp = (uint64_t)sp;

    /* Use kernel page table for now */
    proc->cr3 = virt_to_phys((uint64_t)vmm_get_kernel_pml4());

    /* Set RSP0 in TSS for this process */
    struct tss *tss = get_tss();
    tss->rsp[0] = proc->stack_top;

    enqueue(proc);

    vga_printf("Scheduler: Created process PID=%u\n", (uint32_t)proc->pid);
    return proc->pid;
}

void scheduler_tick(struct interrupt_frame *frame) {
    if (!current_proc) {
        /* No process running, try to schedule one */
        process_t *next = dequeue();
        if (next) {
            current_proc = next;
            current_proc->state = PROC_RUNNING;
            current_proc->remaining_ticks = TIME_SLICE;
        }
        return;
    }

    current_proc->remaining_ticks--;
    if (current_proc->remaining_ticks > 0) return;

    /* Time slice expired - save context and switch */
    process_t *prev = current_proc;
    process_t *next = dequeue();

    if (!next) {
        /* No other process ready, continue current */
        prev->remaining_ticks = TIME_SLICE;
        return;
    }

    /* Save current process RSP from interrupt frame */
    prev->rsp = frame->rsp;
    prev->state = PROC_READY;
    enqueue(prev);

    /* Switch to next process */
    current_proc = next;
    next->state = PROC_RUNNING;
    next->remaining_ticks = TIME_SLICE;

    /* Update TSS RSP0 */
    struct tss *tss = get_tss();
    tss->rsp[0] = next->stack_top;

    /* Switch page table if needed */
    if (prev->cr3 != next->cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(next->cr3) : "memory");
    }

    /* Restore next process RSP */
    frame->rsp = next->rsp;
}

void scheduler_block(void) {
    if (!current_proc) return;
    current_proc->state = PROC_BLOCKED;

    /* Force a reschedule */
    process_t *next = dequeue();
    if (next) {
        process_t *prev = current_proc;
        current_proc = next;
        next->state = PROC_RUNNING;
        next->remaining_ticks = TIME_SLICE;

        struct tss *tss = get_tss();
        tss->rsp[0] = next->stack_top;

        if (prev->cr3 != next->cr3) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(next->cr3) : "memory");
        }
    }
}

void scheduler_unblock(uint64_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].pid == pid && processes[i].state == PROC_BLOCKED) {
            processes[i].state = PROC_READY;
            processes[i].remaining_ticks = TIME_SLICE;
            enqueue(&processes[i]);
            return;
        }
    }
}

process_t *scheduler_current(void) {
    return current_proc;
}
