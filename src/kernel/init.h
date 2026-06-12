/* SpiritFoxOS - 初始化进程接口
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
#ifndef INIT_H
#define INIT_H

#include <stdint.h>

/* 运行init进程（第一个用户空间进程） */
void init_process(void);

/* 运行安全模式（关键设备失败时的最小化环境） */
void safe_mode_run(void);

#endif /* INIT_H */
