#include "hal.h"
#include "memory.h"
#include "string.h"
#include "mmu.h"

void hal_map_2mb(uintptr_t virt, uintptr_t phys, uint64_t flags)
{
    uint64_t *pml4 = (uint64_t *)(hal_read_cr3() & PTE_ADDR_MASK);

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;

    int need_tlb_flush = 0;

    /* Walk PML4 -> PDPT */
    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        void *new_page = alloc_page();
        if (!new_page) return;
        memset(new_page, 0, PAGE_SIZE);
        pml4[pml4_idx] = (uintptr_t)new_page | PTE_PRESENT | PTE_WRITABLE;
        need_tlb_flush = 1;
    }
    uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & PTE_ADDR_MASK);

    /* Walk PDPT -> PD */
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
        void *new_page = alloc_page();
        if (!new_page) return;
        memset(new_page, 0, PAGE_SIZE);
        pdpt[pdpt_idx] = (uintptr_t)new_page | PTE_PRESENT | PTE_WRITABLE;
        need_tlb_flush = 1;
    }
    uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & PTE_ADDR_MASK);

    /* Map 2MB page in PD */
    pd[pd_idx] = phys | flags | PTE_HUGE;

    if (need_tlb_flush) {
        hal_flush_tlb();
    } else {
        hal_flush_tlb_page(virt);
    }
}

void hal_map_4kb(uintptr_t virt, uintptr_t phys, uint64_t flags)
{
    /* Use unified MMU API for 4KB page mapping */
    uint64_t *pte = mmu_walk_page(hal_read_cr3() & PTE_ADDR_MASK, virt, 1);
    if (!pte) return;

    *pte = phys | flags | PTE_PRESENT;
    hal_flush_tlb_page(virt);
}

void hal_ensure_mapped(uintptr_t phys, size_t size)
{
    uintptr_t start = phys & ~(uintptr_t)0x1FFFFF;
    uintptr_t end   = (phys + size + 0x1FFFFF) & ~(uintptr_t)0x1FFFFF;

    for (uintptr_t addr = start; addr < end; addr += 0x200000) {
        hal_map_2mb(addr, addr, PTE_PRESENT | PTE_WRITABLE);
    }
}

void hal_ensure_mapped_mmio(uintptr_t phys, size_t size)
{
    uintptr_t start = phys & ~(uintptr_t)0x1FFFFF;
    uintptr_t end   = (phys + size + 0x1FFFFF) & ~(uintptr_t)0x1FFFFF;

    for (uintptr_t addr = start; addr < end; addr += 0x200000) {
        hal_map_2mb(addr, addr, PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);
    }
}

/* Map a 1GB page at virtual address to physical address using PDPT entry */
void hal_map_1gb(uintptr_t virt, uintptr_t phys, uint64_t flags)
{
    uint64_t *pml4 = (uint64_t *)(hal_read_cr3() & PTE_ADDR_MASK);

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;

    /* Walk PML4 -> PDPT */
    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        void *new_page = alloc_page();
        if (!new_page) return;
        memset(new_page, 0, PAGE_SIZE);
        pml4[pml4_idx] = (uintptr_t)new_page | PTE_PRESENT | PTE_WRITABLE;
    }
    uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & PTE_ADDR_MASK);

    /* Map 1GB page directly in PDPT (PTE_HUGE at PDPT level = 1GB page) */
    pdpt[pdpt_idx] = phys | flags | PTE_HUGE | PTE_PRESENT;

    hal_flush_tlb_page(virt);
}
