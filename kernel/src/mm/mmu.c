#include <mmu.h>
#include <memory.h>
#include <smp.h>
#include <string.h>

/* 页表操作自旋锁（多核安全）
 * 保护 mmu_walk_page (create=1) 和 mmu_map_page 中对共享页表的修改。
 * 注意：mmu_walk_page(create=1) 会调用 alloc_page()，
 * 实际锁序为 mmu_lock → pmm_lock。这不会死锁，因为
 * pmm_lock 的持有者不会反过来获取 mmu_lock。 */
static spinlock_t mmu_lock;

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

        /* 在PD级别遇到2MB大页时，不能继续遍历到PT。
         * 返回PD项指针，调用者需检查PTE_HUGE标志。 */
        if (level == 2 && (tbl[idx] & PTE_HUGE)) {
            return &tbl[idx];
        }

        tbl = (uint64_t *)(tbl[idx] & PTE_ADDR_MASK);
    }
    return &tbl[indices[3]];  // 返回指向PTE的指针
}

int mmu_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags)
{
    spinlock_acquire(&mmu_lock);
    uint64_t *pte = mmu_walk_page(pml4_phys, virt, 1);
    if (!pte) {
        spinlock_release(&mmu_lock);
        return -1;
    }
    *pte = phys | flags | PTE_PRESENT;
    spinlock_release(&mmu_lock);
    return 0;
}

uint64_t mmu_virt_to_phys(uint64_t pml4_phys, uint64_t virt)
{
    /* 手动遍历页表以正确处理大页 */
    uint64_t *tbl = (uint64_t *)(uintptr_t)pml4_phys;

    /* PML4 */
    int pml4_idx = (int)(virt >> 39) & 0x1FF;
    if (!(tbl[pml4_idx] & PTE_PRESENT)) return 0;
    tbl = (uint64_t *)(tbl[pml4_idx] & PTE_ADDR_MASK);

    /* PDPT - 检查1GB大页 */
    int pdpt_idx = (int)(virt >> 30) & 0x1FF;
    if (!(tbl[pdpt_idx] & PTE_PRESENT)) return 0;
    if (tbl[pdpt_idx] & PTE_HUGE) {
        /* 1GB大页：物理地址在位30及以上 */
        uint64_t phys_base = tbl[pdpt_idx] & 0x000FFFFFC0000000ULL;
        return phys_base + (virt & 0x3FFFFFFFULL);
    }
    tbl = (uint64_t *)(tbl[pdpt_idx] & PTE_ADDR_MASK);

    /* PD - 检查2MB大页 */
    int pd_idx = (int)(virt >> 21) & 0x1FF;
    if (!(tbl[pd_idx] & PTE_PRESENT)) return 0;
    if (tbl[pd_idx] & PTE_HUGE) {
        /* 2MB大页：物理地址在位21及以上 */
        uint64_t phys_base = tbl[pd_idx] & 0x000FFFFFFFE00000ULL;
        return phys_base + (virt & 0x1FFFFFULL);
    }
    tbl = (uint64_t *)(tbl[pd_idx] & PTE_ADDR_MASK);

    /* PT - 4KB页 */
    int pt_idx = (int)(virt >> 12) & 0x1FF;
    if (!(tbl[pt_idx] & PTE_PRESENT)) return 0;
    return (tbl[pt_idx] & PTE_ADDR_MASK) + (virt & (PAGE_SIZE - 1));
}

void mmu_init(void)
{
    spinlock_init(&mmu_lock);
}
