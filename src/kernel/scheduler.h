#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "idt.h"

#define MAX_PROCESSES  64
#define STACK_SIZE     (4 * PAGE_SIZE)  /* 16KB */
#define TIME_SLICE     10               /* 10 ticks at 100Hz = 100ms */

/* Forward declaration from pmm.h */
#define PAGE_SIZE 4096

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_ZOMBIE
} proc_state_t;

typedef struct process {
    uint64_t pid;
    proc_state_t state;
    uint64_t rsp;              /* Saved kernel stack pointer */
    uint64_t cr3;              /* Page table (PML4 physical address) */
    uint64_t stack_top;        /* Stack top for deallocation */
    uint64_t remaining_ticks;  /* Remaining time slice */
    struct process *next;      /* Queue link */
} process_t;

void scheduler_init(void);
uint64_t scheduler_create_process(void (*entry)(void));
void scheduler_tick(struct interrupt_frame *frame);
void scheduler_block(void);
void scheduler_unblock(uint64_t pid);
process_t *scheduler_current(void);

#endif /* SCHEDULER_H */
