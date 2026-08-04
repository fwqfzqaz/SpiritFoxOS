#include "syscall_internal.h"
#include "process.h"
#include "memory.h"
#include "hal.h"
#include "string.h"
#include "errno.h"
#include "mmu.h"
#include "serial.h"
#include "chardev.h"
#include "vfs.h"

/* mmap 标志 */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED_VAL ((uint64_t)-1)

/* mmap protectioneflags ction flags */
#define PROT_NONE     0x0
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define PROT_EXEC     0x4

/* ========================================================================
 * 内存系统调用
 * ======================================================================== */

int64_t sys_mmap(trap_frame_t *frame)
{
    uint64_t addr   = frame->rdi;
    size_t   length = (size_t)frame->rsi;
    int      prot   = (int)frame->rdx;
    int      flags  = (int)frame->r10;
    int      fd     = (int)frame->r8;
    int64_t  offset = (int64_t)frame->r9;

    if (length == 0)
        return -EINVAL;

    process_t *proc = process_current();
    if (!proc)
        return -ENOMEM;

    size_t map_len = (length + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    uint64_t map_addr;
    if (addr != 0 && (flags & MAP_FIXED)) {
        map_addr = addr & ~(uint64_t)(PAGE_SIZE - 1);
    } else if (addr != 0) {
        /* 有 hint 地址但没有 MAP_FIXED：尝试在 hint 地址处映射。
         * 如果该区域已被占用，回退到 mmap_current。 */
        uint64_t hint = addr & ~(uint64_t)(PAGE_SIZE - 1);
        int available = 1;
        for (size_t off = 0; off < map_len; off += PAGE_SIZE) {
            uint64_t virt = hint + off;
            uint64_t *pte = mmu_walk_page(proc->pml4, virt, 0);
            if (pte && (*pte & PTE_PRESENT)) {
                available = 0;
                break;
            }
        }
        if (available) {
            map_addr = hint;
        } else {
            if (proc->mmap_current == 0)
                proc->mmap_current = proc->mmap_base;
            map_addr = proc->mmap_current;
            proc->mmap_current += map_len;
        }
    } else {
        if (proc->mmap_current == 0)
            proc->mmap_current = proc->mmap_base;
        map_addr = proc->mmap_current;
        if (map_addr == 0)
            map_addr = 0x7FFFF0000000ULL - 0x40000000ULL;
        proc->mmap_current += map_len;
    }

    uint64_t pte_flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE)
        pte_flags |= PTE_WRITABLE;
    /* PROT_NONE: 页面存在但不可访问 — 访问时触发 page fault。
     * 仍设置 PTE_PRESENT 以便后续 mprotect 可找到并修改权限。 */

    /* 对于非匿名文件映射，尝试使用字符设备 mmap 处理器 */
    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        vfs_file_t **fds = vfs_get_fd_table();
        if (fd >= 0 && fd < VFS_MAX_FD && fds[fd] && fds[fd]->inode &&
            fds[fd]->inode->type == VFS_TYPE_CHARDEV) {
            int ret = chardev_mmap(fds[fd], map_addr, map_len, prot, flags, (uint64_t)offset);
            if (ret == 0)
                return (int64_t)map_addr;
            /* chardev_mmap 失败，回退到匿名映射 */
        }
    }

    /* 对于 MAP_ANONYMOUS 或文件映射，分配零填充页面 */
    if ((flags & MAP_ANONYMOUS) || fd >= 0) {
        for (size_t off = 0; off < map_len; off += PAGE_SIZE) {
            void *phys_page = alloc_page();
            if (!phys_page)
                return (int64_t)map_addr;
            memset(phys_page, 0, PAGE_SIZE);

            uint64_t virt = map_addr + off;
            uint64_t phys = (uint64_t)(uintptr_t)phys_page;

            if (mmu_map_page(proc->pml4, virt, phys, pte_flags) < 0) {
                serial_puts("[mmap] mmu_map_page FAILED for virt=0x");
                serial_put_hex(virt);
                serial_puts("\n");
                return (int64_t)map_addr;
            }
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

    /* 只增长 brk，不收缩（简单实现） */
    if (addr <= proc->brk) {
        return (int64_t)proc->brk;
    }

    /* Allocate physical pages for the newocaterangel pages for the new brk range */
    uint64_t old_page = (proc->brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t new_page = (addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    for (uint64_t vaddr = old_page; vaddr < new_page; vaddr += PAGE_SIZE) {
        /* 检查是否已映射 */
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

    /* addr 必须页对齐 */
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;

    size_t len = (length + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    /* 构建新的 PTE 标志 */
    uint64_t new_flags;
    if (prot == PROT_NONE) {
        /* PROT_NONE: 页面存在但不可访问 — 保留 PTE_PRESENT 以便后续 mprotect
         * 可恢复权限，但移除 PTE_USER 和 PTE_WRITABLE 使用户态访问触发 PF。
         * 内核态访问（如通过页表遍历）不受影响。 */
        new_flags = PTE_PRESENT;
    } else {
        new_flags = PTE_PRESENT | PTE_USER;
        if (prot & PROT_WRITE)
            new_flags |= PTE_WRITABLE;
        /* 注意：在未启用 NX 位的 x86-64 上，PROT_EXEC 不会在 PTE 层级强制执行。
         * 如果添加 NX 支持，PROT_EXEC 应清除 NX 位。 */
    }

    /* 遍历每个页面并更新 PTE 标志 */
    for (size_t off = 0; off < len; off += PAGE_SIZE) {
        uint64_t virt = addr + off;
        uint64_t *pte = mmu_walk_page(proc->pml4, virt, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            uint64_t phys = *pte & ~(uint64_t)(PAGE_SIZE - 1) & ~(uint64_t)0xFFFULL;
            *pte = phys | new_flags;
        }
        /* 如果页面未映射，静默跳过（Linux 在某些情况下允许对未映射区域调用 mprotect） */
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

    /* addr 必须页对齐 */
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;

    size_t len = (length + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    /* 遍历每个页面，释放物理页面并清除 PTE */
    for (size_t off = 0; off < len; off += PAGE_SIZE) {
        uint64_t virt = addr + off;
        uint64_t *pte = mmu_walk_page(proc->pml4, virt, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            /* 释放物理页面 */
            uint64_t phys = *pte & ~(uint64_t)(PAGE_SIZE - 1) & ~(uint64_t)0xFFFULL;
            free_page((void *)(uintptr_t)phys);
            /* 清除 PTE */
            *pte = 0;
        }
    }

    /* Flush TLB for the affected range */
    for (size_t off = 0; off < len; off += PAGE_SIZE * 512) {
        __asm__ volatile("invlpg (%0)" :: "r"(addr + off) : "memory");
    }

    return 0;
}

/* mremap 标志 */
#define MREMAP_MAYMOVE  1
#define MREMAP_FIXED    2

int64_t sys_mremap(trap_frame_t *frame)
{
    uint64_t old_addr   = frame->rdi;
    size_t   old_size   = (size_t)frame->rsi;
    size_t   new_size   = (size_t)frame->rdx;
    int      flags      = (int)frame->r10;

    process_t *proc = process_current();
    if (!proc)
        return -ENOMEM;

    if (new_size == 0 || old_size == 0)
        return -EINVAL;

    /* old_addr 必须页对齐 */
    if (old_addr & (PAGE_SIZE - 1))
        return -EINVAL;

    size_t old_len = (old_size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    size_t new_len = (new_size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    /* 情况 1：收缩 — 释放多余页面 */
    if (new_len <= old_len) {
        for (size_t off = new_len; off < old_len; off += PAGE_SIZE) {
            uint64_t virt = old_addr + off;
            uint64_t *pte = mmu_walk_page(proc->pml4, virt, 0);
            if (pte && (*pte & PTE_PRESENT)) {
                uint64_t phys = *pte & ~(uint64_t)(PAGE_SIZE - 1) & ~(uint64_t)0xFFFULL;
                free_page((void *)(uintptr_t)phys);
                *pte = 0;
            }
        }
        for (size_t off = new_len; off < old_len; off += PAGE_SIZE * 512) {
            __asm__ volatile("invlpg (%0)" :: "r"(old_addr + off) : "memory");
        }
        return (int64_t)old_addr;
    }

    /* 情况 2：原地扩展 — 尝试在旧区域之后直接映射新页面 */
    int can_grow_inplace = 1;
    for (size_t off = old_len; off < new_len; off += PAGE_SIZE) {
        uint64_t virt = old_addr + off;
        uint64_t *pte = mmu_walk_page(proc->pml4, virt, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            can_grow_inplace = 0;
            break;
        }
    }

    if (can_grow_inplace) {
        /* 获取旧区域的权限 */
        uint64_t *old_pte = mmu_walk_page(proc->pml4, old_addr, 0);
        uint64_t pte_flags = PTE_USER | PTE_PRESENT;
        if (old_pte && (*old_pte & PTE_WRITABLE))
            pte_flags |= PTE_WRITABLE;

        for (size_t off = old_len; off < new_len; off += PAGE_SIZE) {
            void *phys_page = alloc_page();
            if (!phys_page)
                return -ENOMEM;
            memset(phys_page, 0, PAGE_SIZE);
            uint64_t virt = old_addr + off;
            if (mmu_map_page(proc->pml4, virt, (uint64_t)(uintptr_t)phys_page, pte_flags) < 0) {
                free_page(phys_page);
                return -ENOMEM;
            }
        }
        return (int64_t)old_addr;
    }

    /* 情况 3：需要移动 — 分配新区域，复制数据，释放旧区域 */
    if (!(flags & MREMAP_MAYMOVE))
        return -ENOMEM;

    if (proc->mmap_current == 0)
        proc->mmap_current = proc->mmap_base;
    uint64_t new_addr = proc->mmap_current;
    proc->mmap_current += new_len;

    /* 获取旧区域权限 */
    uint64_t *old_pte = mmu_walk_page(proc->pml4, old_addr, 0);
    uint64_t pte_flags = PTE_USER | PTE_PRESENT;
    if (old_pte && (*old_pte & PTE_WRITABLE))
        pte_flags |= PTE_WRITABLE;

    /* 映射新区域 */
    for (size_t off = 0; off < new_len; off += PAGE_SIZE) {
        void *phys_page = alloc_page();
        if (!phys_page)
            return -ENOMEM;
        memset(phys_page, 0, PAGE_SIZE);
        if (mmu_map_page(proc->pml4, new_addr + off, (uint64_t)(uintptr_t)phys_page, pte_flags) < 0) {
            free_page(phys_page);
            return -ENOMEM;
        }
    }

    /* 复制旧数据到新区域（按页复制物理内容） */
    size_t copy_len = old_len < new_len ? old_len : new_len;
    for (size_t off = 0; off < copy_len; off += PAGE_SIZE) {
        uint64_t *src_pte = mmu_walk_page(proc->pml4, old_addr + off, 0);
        if (src_pte && (*src_pte & PTE_PRESENT)) {
            uint64_t src_phys = *src_pte & ~(uint64_t)(PAGE_SIZE - 1) & ~(uint64_t)0xFFFULL;
            uint64_t *dst_pte = mmu_walk_page(proc->pml4, new_addr + off, 0);
            if (dst_pte && (*dst_pte & PTE_PRESENT)) {
                uint64_t dst_phys = *dst_pte & ~(uint64_t)(PAGE_SIZE - 1) & ~(uint64_t)0xFFFULL;
                memcpy((void *)(uintptr_t)dst_phys, (void *)(uintptr_t)src_phys, PAGE_SIZE);
            }
        }
    }

    /* 释放旧区域 */
    for (size_t off = 0; off < old_len; off += PAGE_SIZE) {
        uint64_t virt = old_addr + off;
        uint64_t *pte = mmu_walk_page(proc->pml4, virt, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            uint64_t phys = *pte & ~(uint64_t)(PAGE_SIZE - 1) & ~(uint64_t)0xFFFULL;
            free_page((void *)(uintptr_t)phys);
            *pte = 0;
        }
    }
    for (size_t off = 0; off < old_len; off += PAGE_SIZE * 512) {
        __asm__ volatile("invlpg (%0)" :: "r"(old_addr + off) : "memory");
    }

    return (int64_t)new_addr;
}

int64_t sys_madvise(trap_frame_t *frame)
{
    (void)frame;
    return 0;  /* 忽略 madvise 提示 */
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
