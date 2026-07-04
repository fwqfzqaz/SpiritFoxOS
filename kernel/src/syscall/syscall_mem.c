#include "syscall_internal.h"
#include "process.h"
#include "memory.h"
#include "hal.h"
#include "string.h"
#include "errno.h"
#include "mmu.h"

/* mmap flags */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED_VAL ((uint64_t)-1)

/* mmap protection flags */
#define PROT_NONE     0x0
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define PROT_EXEC     0x4

/* ========================================================================
 * Memory syscalls
 * ======================================================================== */

int64_t sys_mmap(trap_frame_t *frame)
{
    uint64_t addr   = frame->rdi;
    size_t   length = (size_t)frame->rsi;
    int      prot   = (int)frame->rdx;
    int      flags  = (int)frame->r10;
    int      fd     = (int)frame->r8;
    int64_t  offset = (int64_t)frame->r9;

    (void)fd;
    (void)offset;

    if (length == 0)
        return -EINVAL;

    process_t *proc = process_current();
    if (!proc)
        return -ENOMEM;

    size_t map_len = (length + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    uint64_t map_addr;
    if (addr != 0 && (flags & MAP_FIXED)) {
        map_addr = addr & ~(uint64_t)(PAGE_SIZE - 1);
    } else {
        if (proc->mmap_current == 0)
            proc->mmap_current = proc->mmap_base;
        map_addr = proc->mmap_current;
        if (map_addr == 0)
            map_addr = 0x7FFFF0000000ULL - 0x40000000ULL;
        proc->mmap_current += map_len;
    }

    uint64_t pte_flags = PTE_USER;
    if (prot & PROT_WRITE)
        pte_flags |= PTE_WRITABLE;

    /* For MAP_ANONYMOUS or file-backed, allocate zero-filled pages */
    if ((flags & MAP_ANONYMOUS) || fd >= 0) {
        for (size_t off = 0; off < map_len; off += PAGE_SIZE) {
            void *phys_page = alloc_page();
            if (!phys_page)
                return (int64_t)map_addr;
            memset(phys_page, 0, PAGE_SIZE);

            uint64_t virt = map_addr + off;
            uint64_t phys = (uint64_t)(uintptr_t)phys_page;

            if (mmu_map_page(proc->pml4, virt, phys, pte_flags) < 0)
                return (int64_t)map_addr;
        }
        return (int64_t)map_addr;
    }

    return (int64_t)MAP_FAILED_VAL;
}

int64_t sys_brk(trap_frame_t *frame)
{
    uint64_t addr = frame->rdi;
    process_t *proc = process_current();
    if (!proc)
        return -ENOMEM;

    if (addr == 0) {
        return (int64_t)proc->brk;
    }

    /* Only grow brk, never shrink (simple implementation) */
    if (addr <= proc->brk) {
        return (int64_t)proc->brk;
    }

    /* Allocate physical pages for the new brk range */
    uint64_t old_page = (proc->brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t new_page = (addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    for (uint64_t vaddr = old_page; vaddr < new_page; vaddr += PAGE_SIZE) {
        /* Check if already mapped */
        if (mmu_virt_to_phys(proc->pml4, vaddr) != 0)
            continue;

        void *phys_page = alloc_page();
        if (!phys_page) {
            proc->brk = vaddr;
            return (int64_t)proc->brk;
        }
        memset(phys_page, 0, PAGE_SIZE);
        mmu_map_page(proc->pml4, vaddr, (uint64_t)(uintptr_t)phys_page,
                     PTE_USER | PTE_WRITABLE | PTE_PRESENT);
    }

    proc->brk = addr;
    return (int64_t)proc->brk;
}

int64_t sys_mprotect(trap_frame_t *frame)
{
    uint64_t addr   = frame->rdi;
    size_t   length = (size_t)frame->rsi;
    int      prot   = (int)frame->rdx;

    process_t *proc = process_current();
    if (!proc)
        return -ENOMEM;

    if (length == 0)
        return -EINVAL;

    /* addr must be page-aligned */
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;

    size_t len = (length + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    /* Build new PTE flags */
    uint64_t new_flags = PTE_USER | PTE_PRESENT;
    if (prot & PROT_WRITE)
        new_flags |= PTE_WRITABLE;
    /* Note: PROT_EXEC is not enforced at PTE level on x86-64 with NX bit
     * disabled. If NX support is added, PROT_EXEC should clear the NX bit. */
    if (prot == PROT_NONE)
        new_flags = 0;  /* Remove all access */

    /* Walk through each page and update PTE flags */
    for (size_t off = 0; off < len; off += PAGE_SIZE) {
        uint64_t virt = addr + off;
        uint64_t *pte = mmu_walk_page(proc->pml4, virt, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            uint64_t phys = *pte & ~(uint64_t)(PAGE_SIZE - 1) & ~(uint64_t)0xFFFULL;
            *pte = phys | new_flags;
        }
        /* If page is not mapped, silently skip (Linux allows mprotect on
         * unmapped regions in some cases) */
    }

    /* Flush TLB for the affected range */
    for (size_t off = 0; off < len; off += PAGE_SIZE * 512) {
        __asm__ volatile("invlpg (%0)" :: "r"(addr + off) : "memory");
    }

    return 0;
}

int64_t sys_munmap(trap_frame_t *frame)
{
    uint64_t addr   = frame->rdi;
    size_t   length = (size_t)frame->rsi;

    process_t *proc = process_current();
    if (!proc)
        return -ENOMEM;

    if (length == 0)
        return -EINVAL;

    /* addr must be page-aligned */
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;

    size_t len = (length + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    /* Walk through each page, free physical pages and clear PTEs */
    for (size_t off = 0; off < len; off += PAGE_SIZE) {
        uint64_t virt = addr + off;
        uint64_t *pte = mmu_walk_page(proc->pml4, virt, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            /* Free the physical page */
            uint64_t phys = *pte & ~(uint64_t)(PAGE_SIZE - 1) & ~(uint64_t)0xFFFULL;
            free_page((void *)(uintptr_t)phys);
            /* Clear the PTE */
            *pte = 0;
        }
    }

    /* Flush TLB for the affected range */
    for (size_t off = 0; off < len; off += PAGE_SIZE * 512) {
        __asm__ volatile("invlpg (%0)" :: "r"(addr + off) : "memory");
    }

    return 0;
}

int64_t sys_madvise(trap_frame_t *frame)
{
    (void)frame;
    return 0;  /* Ignore madvise hints */
}

int64_t sys_mlock(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

int64_t sys_munlock(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

int64_t sys_mlockall(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}

int64_t sys_munlockall(trap_frame_t *frame)
{
    (void)frame;
    return 0;
}
