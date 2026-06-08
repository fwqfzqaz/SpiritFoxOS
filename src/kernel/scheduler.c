#include "scheduler.h"
#include "pmm.h"
#include "vmm.h"
#include "log.h"
#include "gdt.h"
#include "../include/stddef.h"
#include "../include/string.h"
#include <stdint.h>

static process_t processes[MAX_PROCESSES];
static process_t *current_proc = NULL;
static process_t *ready_head = NULL;
static process_t *ready_tail = NULL;
static uint64_t next_pid = 1;
static int scheduler_enabled = 1;

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);

void scheduler_set_enabled(int enabled) {
    scheduler_enabled = enabled;
}

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
        processes[i].app_id = 0;
        processes[i].has_permissions = 0;
        memset(&processes[i].perm_session, 0, sizeof(perm_session_t));
    }
    current_proc = NULL;
    ready_head = NULL;
    ready_tail = NULL;
    next_pid = 1;
}

static uint64_t create_process_internal(void (*entry)(void), uint64_t app_id) {
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
    proc->app_id = app_id;
    proc->has_permissions = 0;
    memset(&proc->perm_session, 0, sizeof(perm_session_t));

    if (app_id > 0) {
        perm_app_entry_t *app = perm_find_app(app_id);
        if (app) {
            if (app->type == APP_SYSTEM) {
                proc->has_permissions = 1;
                proc->perm_session.app_id = app_id;
                proc->perm_session.granted_perms = app->granted_perms;
            } else if (app->active) {
                int rc = perm_app_start(app_id, NULL, &proc->perm_session);
                if (rc == 0) {
                    proc->has_permissions = 1;
                }
            }
        }
    }

    uint64_t stack_phys = pmm_alloc_pages(STACK_SIZE / PAGE_SIZE);
    if (!stack_phys) {
        proc->state = PROC_UNUSED;
        return (uint64_t)-1;
    }
    uint64_t stack_virt = phys_to_virt(stack_phys);
    proc->stack_top = stack_virt + STACK_SIZE;

    uint64_t *sp = (uint64_t *)proc->stack_top;

    *--sp = (uint64_t)entry;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;

    proc->rsp = (uint64_t)sp;

    proc->cr3 = virt_to_phys((uint64_t)vmm_get_kernel_pml4());

    struct tss *tss = get_tss();
    tss->rsp[0] = proc->stack_top;

    enqueue(proc);

    LOG_I("sched", "Scheduler: Created process PID=%u app=%u\n",
               (uint32_t)proc->pid, (uint32_t)app_id);
    return proc->pid;
}

uint64_t scheduler_create_process(void (*entry)(void)) {
    return create_process_internal(entry, 0);
}

uint64_t scheduler_create_process_with_app(void (*entry)(void), uint64_t app_id) {
    return create_process_internal(entry, app_id);
}

void scheduler_tick(struct interrupt_frame *frame) {
    if (!scheduler_enabled) return;

    if (!current_proc) {
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

    process_t *prev = current_proc;
    process_t *next = dequeue();

    if (!next) {
        prev->remaining_ticks = TIME_SLICE;
        return;
    }

    prev->rsp = frame->rsp;
    prev->state = PROC_READY;
    enqueue(prev);

    current_proc = next;
    next->state = PROC_RUNNING;
    next->remaining_ticks = TIME_SLICE;

    struct tss *tss = get_tss();
    tss->rsp[0] = next->stack_top;

    if (prev->cr3 != next->cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(next->cr3) : "memory");
    }

    frame->rsp = next->rsp;
}

void scheduler_block(void) {
    if (!current_proc) return;
    current_proc->state = PROC_BLOCKED;

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

int scheduler_check_perm(perm_flag_t perm) {
    if (!current_proc) return 0;
    if (!current_proc->has_permissions) return 0;
    return perm_check_session(&current_proc->perm_session, perm);
}
