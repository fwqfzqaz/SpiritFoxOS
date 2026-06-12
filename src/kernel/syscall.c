/* SpiritFoxOS - 系统调用处理
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
#include "syscall.h"
#include "idt.h"
#include "log.h"
#include "../include/io.h"

/* 系统调用处理函数表 */
static syscall_handler_t syscall_handlers[SYSCALL_MAX];

/* 默认系统调用处理函数 - 返回-1（ENOSYS） */
static int64_t syscall_default(uint64_t arg0, uint64_t arg1,
                               uint64_t arg2, uint64_t arg3) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    return -1;
}

/* 中断0x80处理函数 - 分发系统调用 */
static void syscall_interrupt_handler(struct interrupt_frame *frame) {
    /* 系统调用约定：
     * RAX = 系统调用号
     * RDI = arg0, RSI = arg1, RDX = arg2, RCX = arg3
     * 返回值在RAX中
     */
    uint64_t syscall_num = frame->rax;
    uint64_t arg0 = frame->rdi;
    uint64_t arg1 = frame->rsi;
    uint64_t arg2 = frame->rdx;
    uint64_t arg3 = frame->rcx;

    if (syscall_num >= SYSCALL_MAX || !syscall_handlers[syscall_num]) {
        frame->rax = (uint64_t)-1;
        return;
    }

    int64_t result = syscall_handlers[syscall_num](arg0, arg1, arg2, arg3);
    frame->rax = (uint64_t)result;
}

/* 基本系统调用实现 */
static int64_t sys_write(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg1; (void)arg2; (void)arg3;
    /* arg0 = 文件描述符(0=stdout), arg1指向缓冲区, arg2为长度 */
    /* 简化实现：直接返回成功 */
    (void)arg0;
    return 0;
}

static int64_t sys_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg1; (void)arg2; (void)arg3;
    /* arg0 = 退出码 */
    LOG_I("syscall", "Process exit with code %u", (uint32_t)arg0);
    return 0;
}

static int64_t sys_getpid(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    return 0; /* 内核shell目前总是PID 0 */
}

static int64_t sys_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    return 0;
}

static int64_t sys_sleep(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg1; (void)arg2; (void)arg3;
    /* arg0 = 毫秒数 */
    extern volatile uint64_t timer_ticks;
    uint64_t target = timer_ticks + (arg0 / 10);
    while (timer_ticks < target) hlt();
    return 0;
}

void syscall_init(void) {
    /* 将所有处理函数初始化为默认处理函数 */
    for (int i = 0; i < SYSCALL_MAX; i++) {
        syscall_handlers[i] = syscall_default;
    }

    /* 注册基本系统调用处理函数 */
    syscall_handlers[SYS_WRITE]   = sys_write;
    syscall_handlers[SYS_EXIT]    = sys_exit;
    syscall_handlers[SYS_GETPID]  = sys_getpid;
    syscall_handlers[SYS_YIELD]   = sys_yield;
    syscall_handlers[SYS_SLEEP]   = sys_sleep;

    /* 注册中断0x80处理函数 */
    idt_register_handler(0x80, syscall_interrupt_handler);

    LOG_I("syscall", "System call interface initialized (int 0x80, %d syscalls)", SYSCALL_MAX);
}

void syscall_register(uint64_t syscall_num, syscall_handler_t handler) {
    if (syscall_num >= SYSCALL_MAX) return;
    syscall_handlers[syscall_num] = handler;
}
