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

    /* Allocate new page table */
    uint64_t phys = pmm_alloc_page();
    if (!phys) return NULL;

    page_table_t *table = (page_table_t *)phys_to_virt(phys);
    /* Zero out the new table */
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

    /* Invalidate TLB entry */
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
    /* Check for 2MB huge page */
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

    /* Copy kernel mappings (higher-half entries) */
    if (kernel_pml4) {
        for (int i = 256; i < 512; i++) {
            pml4->entries[i] = kernel_pml4->entries[i];
        }
    }

    return pml4;
}

void vmm_destroy_pml4(page_table_t *pml4) {
    /* Free user-space page tables only (lower 256 entries) */
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
    /* The boot code already set up identity mapping for first 1GB using 2MB pages.
     * We use the current CR3 as our kernel PML4. */
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    kernel_pml4 = (page_table_t *)phys_to_virt(cr3);

    vga_puts("VMM: Virtual memory manager initialized.\n");
}

page_table_t *vmm_get_kernel_pml4(void) {
    return kernel_pml4;
}
