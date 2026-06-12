/* SpiritFoxOS - 虚拟内存管理
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
#include "vmm.h"
#include "pmm.h"
#include "vga.h"
#include "../include/stddef.h"
#include <stdint.h>

static page_table_t *kernel_pml4 = NULL;

static page_table_t *get_or_create_table(page_table_t *parent, uint64_t idx, uint64_t flags) {
    uint64_t entry = parent->entries[idx];
    if (entry & PTE_PRESENT) {
        return (page_table_t *)phys_to_virt(entry & PTE_ADDR_MASK);
    }

    /* 分配新页表 */
    uint64_t phys = pmm_alloc_page();
    if (!phys) return NULL;

    page_table_t *table = (page_table_t *)phys_to_virt(phys);
    /* 将新表清零 */
    for (int i = 0; i < 512; i++) {
        table->entries[i] = 0;
    }

    parent->entries[idx] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    return table;
}

int vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    page_table_t *pdpt = get_or_create_table(pml4, pml4_idx, PTE_WRITABLE | PTE_USER);
    if (!pdpt) return -1;

    page_table_t *pd = get_or_create_table(pdpt, pdpt_idx, PTE_WRITABLE | PTE_USER);
    if (!pd) return -1;

    page_table_t *pt = get_or_create_table(pd, pd_idx, PTE_WRITABLE | PTE_USER);
    if (!pt) return -1;

    pt->entries[pt_idx] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    return 0;
}

uint64_t vmm_unmap_page(page_table_t *pml4, uint64_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t entry = pml4->entries[pml4_idx];
    if (!(entry & PTE_PRESENT)) return 0;
    page_table_t *pdpt = (page_table_t *)phys_to_virt(entry & PTE_ADDR_MASK);

    entry = pdpt->entries[pdpt_idx];
    if (!(entry & PTE_PRESENT)) return 0;
    page_table_t *pd = (page_table_t *)phys_to_virt(entry & PTE_ADDR_MASK);

    entry = pd->entries[pd_idx];
    if (!(entry & PTE_PRESENT)) return 0;
    page_table_t *pt = (page_table_t *)phys_to_virt(entry & PTE_ADDR_MASK);

    uint64_t pte = pt->entries[pt_idx];
    pt->entries[pt_idx] = 0;

    /* 使TLB条目失效 */
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");

    if (pte & PTE_PRESENT) {
        return pte & PTE_ADDR_MASK;
    }
    return 0;
}

uint64_t vmm_get_physical(page_table_t *pml4, uint64_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t entry = pml4->entries[pml4_idx];
    if (!(entry & PTE_PRESENT)) return 0;
    page_table_t *pdpt = (page_table_t *)phys_to_virt(entry & PTE_ADDR_MASK);

    entry = pdpt->entries[pdpt_idx];
    if (!(entry & PTE_PRESENT)) return 0;
    page_table_t *pd = (page_table_t *)phys_to_virt(entry & PTE_ADDR_MASK);

    entry = pd->entries[pd_idx];
    if (!(entry & PTE_PRESENT)) return 0;
    /* 检查2MB大页 */
    if (entry & PTE_HUGE) {
        return (entry & 0x000FFFFFC0000000ULL) + (virt & 0x1FFFFF);
    }
    page_table_t *pt = (page_table_t *)phys_to_virt(entry & PTE_ADDR_MASK);

    uint64_t pte = pt->entries[pt_idx];
    if (!(pte & PTE_PRESENT)) return 0;
    return pte & PTE_ADDR_MASK;
}

page_table_t *vmm_create_pml4(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return NULL;

    page_table_t *pml4 = (page_table_t *)phys_to_virt(phys);
    for (int i = 0; i < 512; i++) {
        pml4->entries[i] = 0;
    }

    /* 复制内核映射（高半部分条目） */
    if (kernel_pml4) {
        for (int i = 256; i < 512; i++) {
            pml4->entries[i] = kernel_pml4->entries[i];
        }
    }

    return pml4;
}

void vmm_destroy_pml4(page_table_t *pml4) {
    /* 仅释放用户空间页表（低256个条目） */
    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        uint64_t pdpt_entry = pml4->entries[pml4_idx];
        if (!(pdpt_entry & PTE_PRESENT)) continue;

        page_table_t *pdpt = (page_table_t *)phys_to_virt(pdpt_entry & PTE_ADDR_MASK);
        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            uint64_t pd_entry = pdpt->entries[pdpt_idx];
            if (!(pd_entry & PTE_PRESENT)) continue;

            page_table_t *pd = (page_table_t *)phys_to_virt(pd_entry & PTE_ADDR_MASK);
            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                uint64_t pt_entry = pd->entries[pd_idx];
                if (!(pt_entry & PTE_PRESENT)) continue;
                pmm_free_page(pt_entry & PTE_ADDR_MASK);
            }
            pmm_free_page(pd_entry & PTE_ADDR_MASK);
        }
        pmm_free_page(pdpt_entry & PTE_ADDR_MASK);
    }
    pmm_free_page(virt_to_phys((uint64_t)pml4));
}

void vmm_init(void) {
    /*
     * 引导代码已经使用2MB页为前1GB建立了恒等映射。
     * 我们使用当前的CR3作为内核PML4。
     */
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    kernel_pml4 = (page_table_t *)phys_to_virt(cr3);

    vga_puts("VMM: Virtual memory manager initialized.\n");
}

page_table_t *vmm_get_kernel_pml4(void) {
    return kernel_pml4;
}
