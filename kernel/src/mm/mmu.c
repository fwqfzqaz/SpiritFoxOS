#include <mmu.h>
#include <memory.h>
#include <string.h>

uint64_t *mmu_walk_page(uint64_t pml4_phys, uint64_t virt, int create)
{
    uint64_t *tbl = (uint64_t *)(uintptr_t)pml4_phys;
    int indices[4];
    indices[0] = (int)(virt >> 39) & 0x1FF;  // PML4
    indices[1] = (int)(virt >> 30) & 0x1FF;  // PDPT
    indices[2] = (int)(virt >> 21) & 0x1FF;  // PD
    indices[3] = (int)(virt >> 12) & 0x1FF;  // PT

    for (int level = 0; level < 3; level++) {
        int idx = indices[level];
        if (!(tbl[idx] & PTE_PRESENT)) {
            if (!create) return NULL;
            void *new_page = alloc_page();
            if (!new_page) return NULL;
            memset(new_page, 0, PAGE_SIZE);
            tbl[idx] = (uint64_t)(uintptr_t)new_page | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
        tbl = (uint64_t *)(tbl[idx] & PTE_ADDR_MASK);
    }
    return &tbl[indices[3]];  // Return pointer to the PTE
}

int mmu_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pte = mmu_walk_page(pml4_phys, virt, 1);
    if (!pte) return -1;
    *pte = phys | flags | PTE_PRESENT;
    return 0;
}

uint64_t mmu_virt_to_phys(uint64_t pml4_phys, uint64_t virt)
{
    uint64_t *pte = mmu_walk_page(pml4_phys, virt, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    return (*pte & PTE_ADDR_MASK) + (virt & (PAGE_SIZE - 1));
}
