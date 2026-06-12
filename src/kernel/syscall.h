/* SpiritFoxOS - 系统调用接口
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
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* 系统调用号 */
#define SYS_READ     0
#define SYS_WRITE    1
#define SYS_OPEN     2
#define SYS_CLOSE    3
#define SYS_EXIT     4
#define SYS_GETPID   5
#define SYS_YIELD    6
#define SYS_SLEEP    7
#define SYS_PUTS     8
#define SYS_GETCHAR  9
#define SYS_MMAP     10
#define SYS_MUNMAP   11
#define SYS_INFO     12

/* 系统调用处理函数最大数量 */
#define SYSCALL_MAX  32

/* 系统调用处理函数类型：接收系统调用号和最多4个参数，返回结果 */
typedef int64_t (*syscall_handler_t)(uint64_t arg0, uint64_t arg1,
                                     uint64_t arg2, uint64_t arg3);

/* 初始化系统调用接口（注册中断0x80处理函数） */
void syscall_init(void);

/* 为指定系统调用号注册处理函数 */
void syscall_register(uint64_t syscall_num, syscall_handler_t handler);

#endif /* SYSCALL_H */
