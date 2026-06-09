/* SpiritFoxOS - 可编程间隔定时器接口
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
#ifndef PIT_H
#define PIT_H

#include <stdint.h>

#define PIT_CHANNEL0  0x40
#define PIT_CHANNEL1  0x41
#define PIT_CHANNEL2  0x42
#define PIT_COMMAND   0x43

#define PIT_FREQ      1193182  /* Base frequency in Hz */

void pit_init(uint32_t freq);
void pit_set_frequency(uint32_t freq);

#endif /* PIT_H */
