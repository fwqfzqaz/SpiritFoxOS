/* SpiritFoxOS - 可编程间隔定时器驱动
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
#include "pit.h"
#include "../include/io.h"

void pit_init(uint32_t freq) {
    pit_set_frequency(freq);
}

void pit_set_frequency(uint32_t freq) {
    uint16_t divisor = (uint16_t)(PIT_FREQ / freq);

    /* 通道0，低字节/高字节，模式3（方波） */
    outb(PIT_COMMAND, 0x36);

    /* 设置分频值 */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}
