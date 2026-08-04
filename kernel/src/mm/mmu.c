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

/* 拆分2MB大页为512个4KB页。
 * pd_entry: 指向PD项的指针（包含2MB大页映射）
 * 返回: 0成功, -1失败 */
static int split_huge_page(uint64_t *pd_entry)
{
    uint64_t huge_entry = *pd_entry;
    uint64_t phys_base = huge_entry & 0x000FFFFFFFE00000ULL;
    uint64_t orig_flags = huge_entry & ~(uint64_t)(PTE_ADDR_MASK | PTE_HUGE);

    /* 分配新的PT页 */
    void *new_pt = alloc_page();
    if (!new_pt)
        return -1;
    memset(new_pt, 0, PAGE_SIZE);

    /* 填充PT：512个4KB页覆盖原2MB范围。
     * 必须添加 PTE_USER 以确保用户模式可以访问这些页面，
     * 因为进程的地址空间需要用户模式访问权限。 */
    uint64_t *pt = (uint64_t *)new_pt;
    for (int i = 0; i < 512; i++) {
        pt[i] = (phys_base + (uint64_t)i * PAGE_SIZE) |
                (orig_flags & ~PTE_HUGE) | PTE_PRESENT | PTE_USER;
    }

    /* 替换PD项：从2MB大页变为PT指针。
     * 必须添加 PTE_USER 和 PTE_WRITABLE 以确保CPU在
     * 页表遍历时允许用户模式访问此PD项下的所有页面。 */
    *pd_entry = (uint64_t)(uintptr_t)new_pt |
                (orig_flags & ~PTE_HUGE) | PTE_PRESENT | PTE_USER | PTE_WRITABLE;

    return 0;
}

int mmu_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags)
{
    spinlock_acquire(&mmu_lock);
    uint64_t *pte = mmu_walk_page(pml4_phys, virt, 1);
    if (!pte) {
        spinlock_release(&mmu_lock);
        return -1;
    }

    /* 检查是否遇到了2MB大页 - 需要拆分 */
    if (*pte & PTE_HUGE) {
        if (split_huge_page(pte) < 0) {
            spinlock_release(&mmu_lock);
            return -1;
        }
        /* 大页已拆分，刷新该2MB范围的TLB */
        uint64_t virt_base = virt & ~(uint64_t)(2 * 1024 * 1024 - 1);
        for (uint64_t v = virt_base; v < virt_base + (2 * 1024 * 1024); v += PAGE_SIZE) {
            __asm__ volatile("invlpg (%0)" :: "r"(v) : "memory");
        }
        /* 重新遍历获取正确的PTE */
        pte = mmu_walk_page(pml4_phys, virt, 0);
        if (!pte) {
            spinlock_release(&mmu_lock);
            return -1;
        }
    }

    *pte = phys | flags | PTE_PRESENT;

    /* 调试：验证PTE值 (已禁用 — 减少串口输出噪音) */
#if 0
    if (virt >= 0x10000000ULL && virt < 0x20000000ULL) {
        extern void serial_puts(const char *);
        extern void serial_put_hex(uint64_t);
        serial_puts("[mmu_map_page] virt=0x");
        serial_put_hex(virt);
        serial_puts(" pte_val=0x");
        serial_put_hex(*pte);
        serial_puts("\n");
    }
#endif

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
