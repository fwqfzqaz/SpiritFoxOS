/* SpiritFoxOS - 物理内存管理
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
#include "pmm.h"
#include "log.h"
#include "../include/bootinfo.h"
#include "../include/efi.h"
#include "../include/stddef.h"
#include <stdint.h>

static uint64_t *pmm_bitmap = NULL;
static int pmm_bitmap_set = 0;
static uint64_t pmm_total_frames = 0;
static uint64_t pmm_free_frames = 0;
static uint64_t pmm_bitmap_size = 0; /* 位图中的qword数量 */

static void pmm_mark_free(uint64_t base, uint64_t length) {
    uint64_t start = (base + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t end = (base + length) / PAGE_SIZE;
    for (uint64_t i = start; i < end && i < pmm_total_frames; i++) {
        uint64_t idx = i / BITS_PER_QWORD;
        uint64_t bit = i % BITS_PER_QWORD;
        if (pmm_bitmap[idx] & (1ULL << bit)) {
            pmm_bitmap[idx] &= ~(1ULL << bit);
            pmm_free_frames++;
        }
    }
}

static void pmm_mark_used(uint64_t base, uint64_t length) {
    uint64_t start = base / PAGE_SIZE;
    uint64_t end = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = start; i < end && i < pmm_total_frames; i++) {
        uint64_t idx = i / BITS_PER_QWORD;
        uint64_t bit = i % BITS_PER_QWORD;
        if (!(pmm_bitmap[idx] & (1ULL << bit))) {
            pmm_bitmap[idx] |= (1ULL << bit);
            pmm_free_frames--;
        }
    }
}

/*
 * 将EFI内存类型转换为可用性标志。
 * 如果内存可用于一般用途返回1，如果保留则返回0。
 */
static int efi_type_is_available(uint32_t efi_type) {
    switch (efi_type) {
        case EFI_CONVENTIONAL_MEMORY:
            return 1;
        case EFI_ACPI_RECLAIM_MEMORY:   /* ACPI读取表后可回收 */
        case EFI_ACPI_MEMORY_NVS:       /* 可使用但休眠时必须保留 */
            return 1; /* 视为可用 - 操作系统可以管理这些 */
        default:
            return 0; /* 保留、不可用、MMIO等 */
    }
}

void pmm_init(bootinfo_t *bootinfo, uint64_t kernel_start, uint64_t kernel_end) {
    /*
     * 来自UEFI的内存映射：bootinfo->mmap_addr处的EFI_MEMORY_DESCRIPTOR数组。
     * 每个描述符为bootinfo->mmap_entry_size字节（可能大于结构体大小）。
     */
    uint8_t *mmap_base = (uint8_t *)(uintptr_t)bootinfo->mmap_addr;
    uint64_t entry_count = bootinfo->mmap_entry_count;
    uint64_t entry_size = bootinfo->mmap_entry_size;

    /* 步骤1：查找最高的可用物理地址 */
    uint64_t max_addr = 0;

    for (uint64_t i = 0; i < entry_count; i++) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR *)(mmap_base + i * entry_size);
        if (efi_type_is_available(desc->type)) {
            uint64_t end = desc->physical_start + (desc->number_of_pages * PAGE_SIZE);
            if (end > max_addr) max_addr = end;
        }
    }

    /* 步骤2：计算位图大小 */
    pmm_total_frames = (max_addr + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_bitmap_size = (pmm_total_frames + BITS_PER_QWORD - 1) / BITS_PER_QWORD;
    uint64_t bitmap_bytes = pmm_bitmap_size * sizeof(uint64_t);

    LOG_D("pmm", "bitmap: %u bytes, max_addr=%p", (uint32_t)bitmap_bytes, max_addr);

    /* 步骤3：查找一个区域来放置位图本身 */
    pmm_bitmap = NULL;
    for (uint64_t i = 0; i < entry_count; i++) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR *)(mmap_base + i * entry_size);
        if (!efi_type_is_available(desc->type))
            continue;

        uint64_t region_start = desc->physical_start;
        uint64_t region_len = desc->number_of_pages * PAGE_SIZE;
        uint64_t region_end = region_start + region_len;

        /* 页面对齐起始地址 */
        uint64_t aligned_start = (region_start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        /* 如果区域太小则跳过 */
        if (region_end <= aligned_start || region_end - aligned_start < bitmap_bytes) {
            continue;
        }

        /* 尝试在内核之前 */
        if (aligned_start + bitmap_bytes <= kernel_start) {
            pmm_bitmap = (uint64_t *)aligned_start;
            pmm_bitmap_set = 1;
            break;
        }

        /* 尝试在内核之后 */
        uint64_t after_kernel = (kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (after_kernel + bitmap_bytes <= region_end) {
            pmm_bitmap = (uint64_t *)after_kernel;
            pmm_bitmap_set = 1;
            break;
        }
    }

    if (!pmm_bitmap_set) {
        LOG_E("pmm", "Failed to find space for bitmap!\n");
        return;
    }

    /* 步骤4：初始化位图 - 标记所有帧为已使用 */
    for (uint64_t i = 0; i < pmm_bitmap_size; i++) {
        pmm_bitmap[i] = 0xFFFFFFFFFFFFFFFFULL;
    }
    pmm_free_frames = 0;

    /* 步骤5：标记可用内存为空闲 */
    for (uint64_t i = 0; i < entry_count; i++) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR *)(mmap_base + i * entry_size);
        if (efi_type_is_available(desc->type)) {
            pmm_mark_free(desc->physical_start, desc->number_of_pages * PAGE_SIZE);
        }
    }

    /* 步骤6：标记内核为已使用 */
    pmm_mark_used(kernel_start, kernel_end - kernel_start);

    /* 步骤7：标记位图为已使用 */
    pmm_mark_used((uint64_t)pmm_bitmap, bitmap_bytes);

    LOG_I("pmm", "%u MB available (%u pages)\n",
               (uint32_t)(pmm_free_frames * PAGE_SIZE / (1024 * 1024)),
               (uint32_t)pmm_free_frames);
}

uint64_t pmm_alloc_page(void) {
    for (uint64_t i = 0; i < pmm_bitmap_size; i++) {
        if (pmm_bitmap[i] != 0xFFFFFFFFFFFFFFFFULL) {
            uint64_t bit = __builtin_ctzll(~pmm_bitmap[i]);
            pmm_bitmap[i] |= (1ULL << bit);
            pmm_free_frames--;
            return (i * BITS_PER_QWORD + bit) * PAGE_SIZE;
        }
    }
    return 0; /* 内存不足 */
}

void pmm_free_page(uint64_t addr) {
    uint64_t frame = addr / PAGE_SIZE;
    uint64_t idx = frame / BITS_PER_QWORD;
    uint64_t bit = frame % BITS_PER_QWORD;
    if (idx < pmm_bitmap_size && (pmm_bitmap[idx] & (1ULL << bit))) {
        pmm_bitmap[idx] &= ~(1ULL << bit);
        pmm_free_frames++;
    }
}

uint64_t pmm_alloc_pages(uint64_t n) {
    if (n == 0) return 0;
    if (n == 1) return pmm_alloc_page();

    uint64_t consecutive = 0;
    uint64_t start_frame = 0;

    for (uint64_t i = 0; i < pmm_bitmap_size && consecutive < n; i++) {
        for (uint64_t bit = 0; bit < BITS_PER_QWORD && consecutive < n; bit++) {
            uint64_t frame = i * BITS_PER_QWORD + bit;
            if (frame >= pmm_total_frames) break;

            if (!(pmm_bitmap[i] & (1ULL << bit))) {
                if (consecutive == 0) start_frame = frame;
                consecutive++;
            } else {
                consecutive = 0;
            }
        }
    }

    if (consecutive < n) return 0;

    /* 标记页为已使用 */
    for (uint64_t f = start_frame; f < start_frame + n; f++) {
        uint64_t idx = f / BITS_PER_QWORD;
        uint64_t bit = f % BITS_PER_QWORD;
        pmm_bitmap[idx] |= (1ULL << bit);
    }
    pmm_free_frames -= n;
    return start_frame * PAGE_SIZE;
}

uint64_t pmm_free_count(void) {
    return pmm_free_frames;
}

uint64_t pmm_total_count(void) {
    return pmm_total_frames;
}
