/* SpiritFoxOS - 进程调度器接口
 * Copyright (C) 2025 SpiritFoxOS Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "idt.h"
#include "perm.h"

#define MAX_PROCESSES  64
#define STACK_SIZE     (4 * PAGE_SIZE)
#define TIME_SLICE     10

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
    uint64_t rsp;
    uint64_t cr3;
    uint64_t stack_top;
    uint64_t remaining_ticks;
    struct process *next;
    uint64_t app_id;
    perm_session_t perm_session;
    int has_permissions;
} process_t;

void scheduler_init(void);
uint64_t scheduler_create_process(void (*entry)(void));
uint64_t scheduler_create_process_with_app(void (*entry)(void), uint64_t app_id);
void scheduler_tick(struct interrupt_frame *frame);
void scheduler_block(void);
void scheduler_unblock(uint64_t pid);
process_t *scheduler_current(void);
int scheduler_check_perm(perm_flag_t perm);
void scheduler_set_enabled(int enabled);

#endif
