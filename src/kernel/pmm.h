/* SpiritFoxOS - 物理内存管理接口
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
#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include "../include/bootinfo.h"

#define PAGE_SIZE 4096
#define BITS_PER_QWORD 64

void pmm_init(bootinfo_t *bootinfo, uint64_t kernel_start, uint64_t kernel_end);
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t addr);
uint64_t pmm_alloc_pages(uint64_t n);
uint64_t pmm_free_count(void);
uint64_t pmm_total_count(void);

#endif /* PMM_H */
