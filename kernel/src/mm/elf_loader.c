/*
 * SpiritFoxOS ELF64 可执行文件加载器
 *
 * 将 ELF64 可执行文件加载到进程的地址空间中，创建
 * 必要的页表，并以适当的权限映射段，供用户态执行。
 */

#include <stddef.h>
#include "process.h"
#include "elf64.h"
#include "hal.h"
#include "memory.h"
#include "kmalloc.h"
#include "string.h"
#include "vfs.h"
#include "vga.h"

/* ========================================================================
 * 页表遍历辅助函数
 * ======================================================================== */

/* 返回在页表 `table_phys` 中映射 `virt` 的指定层级页表项的虚拟地址。
 * 如果下一级页表不存在，则分配一个（恒等映射）。
 * `level` 为 0=PML4, 1=PDPT, 2=PD, 3=PT（从第0级向下遍历）。 */

static uint64_t *walk_entry(uint64_t table_phys, uint64_t virt, int level)
{
    /* 每一级使用9位索引 */
    static const int shifts[4] = { 39, 30, 21, 12 };
    int idx = (int)((virt >> shifts[level]) & 0x1FF);

    uint64_t *table = (uint64_t *)(uintptr_t)table_phys;
    uint64_t entry  = table[idx];

    /* 如果页表项存在，检查是否需要拆分大页 */
    if (entry & PTE_PRESENT) {
        /* 在PD级别（第2级），必须将2MB大页拆分为512个4KB页，
         * 然后才能映射单个4KB页。 */
        if (level == 2 && (entry & PTE_HUGE)) {
            /* 将2MB大页拆分为包含512个4KB页表项的PT */
            void *new_pt = alloc_page();
            if (!new_pt)
                return NULL;
            memset(new_pt, 0, PAGE_SIZE);

            uint64_t huge_phys = entry & 0x000FFFFFFFE00000ULL; /* 2MB对齐的物理地址（第21位及以上） */
            /* 提取标志位：除物理地址和PS位之外的所有位。
             * 添加PTE_USER以使用户态代码可以访问这些页
             *（原始内核2MB大页缺少PTE_USER）。 */
            uint64_t huge_flags = entry & ~0x000FFFFFFFE00000ULL;
            huge_flags &= ~PTE_HUGE;  /* 清除页面大小位 */
            huge_flags |= PTE_USER;   /* 允许用户态访问 */

            uint64_t *pt = (uint64_t *)new_pt;
            for (int i = 0; i < 512; i++) {
                pt[i] = (huge_phys + (uint64_t)i * PAGE_SIZE) | huge_flags;
            }

            /* 替换PD项：指向新的PT而非2MB大页 */
            uint64_t pt_phys = (uint64_t)(uintptr_t)new_pt;
            table[idx] = pt_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

            /* 返回PD项，以便调用者继续向下遍历 */
            return &table[idx];
        }

        return &table[idx];
    }

    /* 需要为下一级分配新的页表。
     *（在最后一级——PT——我们不会调用此函数来分配，
     *  而是直接设置最终的PTE。） */
    void *new_page = alloc_page();
    if (!new_page)
        return NULL;

    memset(new_page, 0, PAGE_SIZE);

    /* 新页面使用恒等映射，因此物理地址 == 虚拟地址 */
    uint64_t new_phys = (uint64_t)(uintptr_t)new_page;

    /* 设置页表项：存在、可写、用户可访问 */
    table[idx] = new_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    return &table[idx];
}

/* ========================================================================
 * map_user_page – 在进程地址空间中映射单个4KB页
 * ======================================================================== */

static int map_user_page(process_t *proc, uint64_t virt, uint64_t phys,
                         uint64_t flags)
{
    uint64_t pml4_phys = proc->pml4;

    /* 遍历 PML4 -> PDPT -> PD，按需分配页表 */
    uint64_t *pml4e = walk_entry(pml4_phys, virt, 0);
    if (!pml4e) return -1;

    uint64_t pdpt_phys = *pml4e & PTE_ADDR_MASK;
    uint64_t *pdpte = walk_entry(pdpt_phys, virt, 1);
    if (!pdpte) return -2;

    uint64_t pd_phys = *pdpte & PTE_ADDR_MASK;
    uint64_t *pde = walk_entry(pd_phys, virt, 2);
    if (!pde) return -3;

    /* 在PD级别，页表项指向PT */
    uint64_t pt_phys;
    if (*pde & PTE_PRESENT) {
        pt_phys = *pde & PTE_ADDR_MASK;
    } else {
        void *pt_page = alloc_page();
        if (!pt_page) return -4;
        memset(pt_page, 0, PAGE_SIZE);
        pt_phys = (uint64_t)(uintptr_t)pt_page;
        *pde = pt_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    /* 设置最终的PTE */
    int pt_idx = (int)((virt >> 12) & 0x1FF);
    uint64_t *pt = (uint64_t *)(uintptr_t)pt_phys;
    pt[pt_idx] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;

    return 0;
}

/* ========================================================================
 * map_user_range – 映射一段连续的4KB虚拟页范围，
 *                  为每个虚拟页分配一个物理页
 * ======================================================================== */

static uint64_t map_user_range(process_t *proc, uint64_t virt, size_t size,
                               uint64_t flags)
{
    uint64_t start = virt;
    uint64_t end   = (virt + size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        void *phys_page = alloc_page();
        if (!phys_page)
            return 0;  /* 失败 – 内存不足 */

        memset(phys_page, 0, PAGE_SIZE);

        uint64_t phys = (uint64_t)(uintptr_t)phys_page;
        if (map_user_page(proc, v, phys, flags) != 0)
            return 0;
    }

    return start;
}

/* ========================================================================
 * create_user_pml4 – 分配新的PML4并复制内核映射
 * ======================================================================== */

/* 深拷贝页表：分配新页面，复制所有页表项。
 * 这对于低半部分PML4项是必要的，以便在进程副本中
 * 拆分大页时不会影响内核。 */
static uint64_t deep_copy_table(uint64_t src_table_phys)
{
    void *new_table = alloc_page();
    if (!new_table)
        return 0;
    memcpy(new_table, (void *)(uintptr_t)src_table_phys, PAGE_SIZE);
    return (uint64_t)(uintptr_t)new_table;
}

static uint64_t create_user_pml4(void)
{
    void *new_pml4 = alloc_page();
    if (!new_pml4)
        return 0;

    uint64_t *dst = (uint64_t *)new_pml4;
    uint64_t *src = (uint64_t *)(uintptr_t)hal_read_cr3();

    /* 从内核页表复制所有512个PML4项。
     * 内核在低半部分（PML4[0..255]）使用恒等映射
     * 用于代码、数据、堆和MMIO。只复制高半部分
     *（PML4[256..511]）会丢失所有内核映射。
     *
     * 对于低半部分的PML4项（用户空间范围），我们
     * 深拷贝PDPT，以便在进程页表中拆分大页时
     * 不会影响内核的映射。
     * 对于高半部分（仅内核使用），浅拷贝是安全的。 */
    for (int i = 0; i < 256; i++) {
        if (src[i] & PTE_PRESENT) {
            /* 深拷贝此PML4项对应的PDPT */
            uint64_t pdpt_phys = src[i] & PTE_ADDR_MASK;
            uint64_t new_pdpt = deep_copy_table(pdpt_phys);
            if (!new_pdpt) {
                /* 回退：浅拷贝 */
                dst[i] = src[i];
            } else {
                /* 从原始项复制标志位，但添加 USER | WRITABLE
                 * 以使用户态代码可以遍历这些页表。
                 * 实际的页面权限在PT级别强制执行。 */
                dst[i] = new_pdpt | (src[i] & ~PTE_ADDR_MASK) | PTE_USER | PTE_WRITABLE;
            }

            /* 同时深拷贝PDPT中的每个PD，因为
             * 我们可能需要将2MB大页拆分为4KB页。
             * 同样为PD项添加 USER | WRITABLE。 */
            uint64_t *pdpt = (uint64_t *)(new_pdpt ? new_pdpt : pdpt_phys);
            for (int j = 0; j < 512; j++) {
                if (pdpt[j] & PTE_PRESENT) {
                    uint64_t pd_phys = pdpt[j] & PTE_ADDR_MASK;
                    uint64_t new_pd = deep_copy_table(pd_phys);
                    if (new_pd) {
                        pdpt[j] = new_pd | (pdpt[j] & ~PTE_ADDR_MASK) | PTE_USER | PTE_WRITABLE;
                    }
                }
            }
        } else {
            dst[i] = 0;
        }
    }

    /* 高半部分：浅拷贝（仅内核使用，无需拆分） */
    for (int i = 256; i < 512; i++)
        dst[i] = src[i];

    return (uint64_t)(uintptr_t)new_pml4;
}

/* ========================================================================
 * elf_load – 将ELF64可执行文件加载到进程地址空间
 *
 * 返回入口点地址，失败时返回0。
 * ======================================================================== */

uint64_t elf_load(process_t *proc, const void *data, size_t size)
{
    /* ---- 验证ELF头部 ---- */
    if (size < sizeof(Elf64_Ehdr))
        return 0;

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;

    /* 魔数 */
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3)
        return 0;

    /* 类别 */
    if (ehdr->e_ident[4] != ELFCLASS64)
        return 0;

    /* 数据编码 */
    if (ehdr->e_ident[5] != ELFDATA2LSB)
        return 0;

    /* 机器架构 */
    if (ehdr->e_machine != EM_X86_64)
        return 0;

    /* 类型：可执行文件或共享对象（PIE） */
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        return 0;

    /* 必须有程序头表 */
    if (ehdr->e_phnum == 0 || ehdr->e_phoff == 0)
        return 0;

    /* 完整性检查：程序头表必须包含在数据中 */
    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > size)
        return 0;

    /* ---- 为进程创建新的PML4 ---- */
    uint64_t pml4 = create_user_pml4();
    if (!pml4)
        return 0;

    proc->pml4 = pml4;

    /* ---- 加载 PT_LOAD 段 ---- */
    uint64_t highest_addr = 0;
    const Elf64_Phdr *phdr = (const Elf64_Phdr *)
        ((const uint8_t *)data + ehdr->e_phoff);

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        uint64_t vaddr  = phdr[i].p_vaddr;
        uint64_t filesz = phdr[i].p_filesz;
        uint64_t memsz  = phdr[i].p_memsz;
        uint64_t offset = phdr[i].p_offset;
        uint64_t flags  = PTE_USER;

        if (phdr[i].p_flags & PF_W)
            flags |= PTE_WRITABLE;

        /* 确保段数据包含在文件中 */
        if (offset + filesz > size) {
            printf("elf_load: segment %u file data out of bounds\n", i);
            return 0;
        }

        /* 页面对齐虚拟地址范围 */
        uint64_t seg_start = vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t seg_end   = (vaddr + memsz + PAGE_SIZE - 1) &
                             ~(uint64_t)(PAGE_SIZE - 1);
        size_t   seg_size  = (size_t)(seg_end - seg_start);

        /* 映射范围，分配物理页 */
        if (!map_user_range(proc, seg_start, seg_size, flags)) {
            printf("elf_load: failed to map segment %u at 0x%lx\n",
                   i, seg_start);
            return 0;
        }

        /* 将文件数据复制到已映射的页面中。
         * 由于内核使用恒等映射，且用户页面是恒等映射的
         * 物理页面，我们可以直接通过物理地址写入。
         * 通过进程的PML4遍历页表来找到每个物理页。 */
        uint64_t file_remaining = filesz;
        uint64_t src_off       = offset;
        uint64_t dst_vaddr     = vaddr;

        while (file_remaining > 0) {
            uint64_t page_vaddr = dst_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t page_off   = dst_vaddr & (PAGE_SIZE - 1);
            size_t   chunk      = PAGE_SIZE - (size_t)page_off;
            if (chunk > file_remaining)
                chunk = (size_t)file_remaining;

            /* 遍历进程的页表以查找物理页 */
            uint64_t *pml4_tbl = (uint64_t *)(uintptr_t)proc->pml4;
            int pml4_idx = (int)(page_vaddr >> 39) & 0x1FF;
            if (!(pml4_tbl[pml4_idx] & PTE_PRESENT)) {
                printf("elf_load: PML4 entry not present for 0x%lx\n",
                       page_vaddr);
                return 0;
            }

            uint64_t *pdpt = (uint64_t *)(pml4_tbl[pml4_idx] & PTE_ADDR_MASK);
            int pdpt_idx = (int)(page_vaddr >> 30) & 0x1FF;
            if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
                printf("elf_load: PDPT entry not present for 0x%lx\n",
                       page_vaddr);
                return 0;
            }

            uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & PTE_ADDR_MASK);
            int pd_idx = (int)(page_vaddr >> 21) & 0x1FF;
            if (!(pd[pd_idx] & PTE_PRESENT)) {
                printf("elf_load: PD entry not present for 0x%lx\n",
                       page_vaddr);
                return 0;
            }

            uint64_t *pt = (uint64_t *)(pd[pd_idx] & PTE_ADDR_MASK);
            int pt_idx = (int)(page_vaddr >> 12) & 0x1FF;
            if (!(pt[pt_idx] & PTE_PRESENT)) {
                printf("elf_load: PT entry not present for 0x%lx\n",
                       page_vaddr);
                return 0;
            }

            uint64_t phys_page = pt[pt_idx] & PTE_ADDR_MASK;
            void *dst = (void *)(uintptr_t)(phys_page + page_off);

            memcpy(dst, (const uint8_t *)data + src_off, chunk);

            src_off       += chunk;
            dst_vaddr     += chunk;
            file_remaining -= chunk;
        }

        /* 将BSS部分清零（memsz > filesz）。
         * 物理页面已由map_user_range清零，
         * 因此只需清除边界处的部分页面。 */
        if (memsz > filesz) {
            uint64_t bss_start = vaddr + filesz;
            uint64_t bss_end   = vaddr + memsz;

            /* BSS的第一页可能与最后一个文件页重叠 */
            uint64_t bss_page = bss_start & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t bss_off  = bss_start & (PAGE_SIZE - 1);
            size_t   first_chunk = PAGE_SIZE - (size_t)bss_off;

            /* 遍历页表找到第一个部分页 */
            uint64_t *pml4_tbl = (uint64_t *)(uintptr_t)proc->pml4;
            int pml4_idx = (int)(bss_page >> 39) & 0x1FF;
            uint64_t *pdpt_bss = (uint64_t *)(pml4_tbl[pml4_idx] & PTE_ADDR_MASK);
            int pdpt_idx = (int)(bss_page >> 30) & 0x1FF;
            uint64_t *pd_bss = (uint64_t *)(pdpt_bss[pdpt_idx] & PTE_ADDR_MASK);
            int pd_idx = (int)(bss_page >> 21) & 0x1FF;
            uint64_t *pt_bss = (uint64_t *)(pd_bss[pd_idx] & PTE_ADDR_MASK);
            int pt_idx = (int)(bss_page >> 12) & 0x1FF;
            uint64_t phys_bss = pt_bss[pt_idx] & PTE_ADDR_MASK;

            if (first_chunk > (size_t)(bss_end - bss_start))
                first_chunk = (size_t)(bss_end - bss_start);

            memset((void *)(uintptr_t)(phys_bss + bss_off), 0, first_chunk);

            /* 其余完整页面已由map_user_range中的alloc_page/memset清零，
             * 无需额外操作。 */
        }

        /* 跟踪最高地址，用于brk */
        if (vaddr + memsz > highest_addr)
            highest_addr = vaddr + memsz;
    }

    /* ---- 设置进程字段 ---- */
    proc->entry_point = ehdr->e_entry;
    proc->brk         = (highest_addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    proc->brk        += 0x100000;  /* 1 MB 堆增长空间 */

    /* ---- 映射用户栈：4 MB，位于 0x7FFFF0000000 向下 ---- */
    uint64_t stack_top = 0x7FFFF0000000ULL;
    uint64_t stack_base = stack_top - PROC_STACK_SIZE;  /* 向下4 MB */

    if (!map_user_range(proc, stack_base, PROC_STACK_SIZE,
                        PTE_USER | PTE_WRITABLE)) {
        printf("elf_load: failed to map user stack\n");
        return 0;
    }

    proc->stack_top = stack_top;

    /* ---- 设置 mmap_base（供未来的 mmap 实现） ---- */
    proc->mmap_base = proc->brk + 0x10000000ULL;  /* brk 之后 256 MB */

    return proc->entry_point;
}

/* ========================================================================
 * elf_load_with_base – 以基地址偏移加载ELF文件
 *
 * 类似于 elf_load()，但将所有 PT_LOAD 段映射到 base_addr + p_vaddr
 * 而不是仅映射到 p_vaddr。这对于 PIE 可执行文件（ET_DYN）
 * 和共享库（动态链接器）是必需的。
 *
 * 如果 proc->pml4 已设置，则复用它（用于在主二进制文件
 * 之后加载解释器）。否则创建新的PML4。
 *
 * 如果 proc->stack_top 为 0（首次加载），则映射用户栈并
 * 设置进程字段。否则仅加载 PT_LOAD 段。
 *
 * 成功返回0，失败返回负值。
 * ======================================================================== */

int elf_load_with_base(process_t *proc, const void *data, size_t size,
                       uint64_t base_addr)
{
    /* ---- 验证ELF头部 ---- */
    if (size < sizeof(Elf64_Ehdr))
        return -1;

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;

    /* 魔数 */
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3)
        return -1;

    /* 类别 */
    if (ehdr->e_ident[4] != ELFCLASS64)
        return -1;

    /* 数据编码 */
    if (ehdr->e_ident[5] != ELFDATA2LSB)
        return -1;

    /* 机器架构 */
    if (ehdr->e_machine != EM_X86_64)
        return -1;

    /* 类型：可执行文件或共享对象（PIE） */
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        return -1;

    /* 必须有程序头表 */
    if (ehdr->e_phnum == 0 || ehdr->e_phoff == 0)
        return -1;

    /* 完整性检查：程序头表必须包含在数据中 */
    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > size)
        return -1;

    /* ---- 创建或复用PML4 ---- */
    if (!proc->pml4) {
        uint64_t pml4 = create_user_pml4();
        if (!pml4)
            return -2;
        proc->pml4 = pml4;
    }

    /* ---- 加载 PT_LOAD 段 ---- */
    uint64_t highest_addr = 0;
    const Elf64_Phdr *phdr = (const Elf64_Phdr *)
        ((const uint8_t *)data + ehdr->e_phoff);

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        /* 对于 ET_DYN，加上 base_addr；对于 ET_EXEC，base_addr 应为0 */
        uint64_t vaddr  = phdr[i].p_vaddr + base_addr;
        uint64_t filesz = phdr[i].p_filesz;
        uint64_t memsz  = phdr[i].p_memsz;
        uint64_t offset = phdr[i].p_offset;
        uint64_t flags  = PTE_USER;

        if (phdr[i].p_flags & PF_W)
            flags |= PTE_WRITABLE;

        /* 确保段数据包含在文件中 */
        if (offset + filesz > size) {
            printf("elf_load_with_base: segment %u file data out of bounds\n", i);
            return -3;
        }

        /* 页面对齐虚拟地址范围 */
        uint64_t seg_start = vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t seg_end   = (vaddr + memsz + PAGE_SIZE - 1) &
                             ~(uint64_t)(PAGE_SIZE - 1);
        size_t   seg_size  = (size_t)(seg_end - seg_start);

        /* 映射范围，分配物理页 */
        if (!map_user_range(proc, seg_start, seg_size, flags)) {
            printf("elf_load_with_base: failed to map segment %u at 0x%lx\n",
                   i, seg_start);
            return -4;
        }

        /* 将文件数据复制到已映射的页面 */
        uint64_t file_remaining = filesz;
        uint64_t src_off       = offset;
        uint64_t dst_vaddr     = vaddr;

        while (file_remaining > 0) {
            uint64_t page_vaddr = dst_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t page_off   = dst_vaddr & (PAGE_SIZE - 1);
            size_t   chunk      = PAGE_SIZE - (size_t)page_off;
            if (chunk > file_remaining)
                chunk = (size_t)file_remaining;

            /* 遍历进程的页表以查找物理页 */
            uint64_t *pml4_tbl = (uint64_t *)(uintptr_t)proc->pml4;
            int pml4_idx = (int)(page_vaddr >> 39) & 0x1FF;
            if (!(pml4_tbl[pml4_idx] & PTE_PRESENT)) {
                printf("elf_load_with_base: PML4 entry not present for 0x%lx\n",
                       page_vaddr);
                return -5;
            }

            uint64_t *pdpt = (uint64_t *)(pml4_tbl[pml4_idx] & PTE_ADDR_MASK);
            int pdpt_idx = (int)(page_vaddr >> 30) & 0x1FF;
            if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
                printf("elf_load_with_base: PDPT entry not present for 0x%lx\n",
                       page_vaddr);
                return -5;
            }

            uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & PTE_ADDR_MASK);
            int pd_idx = (int)(page_vaddr >> 21) & 0x1FF;
            if (!(pd[pd_idx] & PTE_PRESENT)) {
                printf("elf_load_with_base: PD entry not present for 0x%lx\n",
                       page_vaddr);
                return -5;
            }

            uint64_t *pt = (uint64_t *)(pd[pd_idx] & PTE_ADDR_MASK);
            int pt_idx = (int)(page_vaddr >> 12) & 0x1FF;
            if (!(pt[pt_idx] & PTE_PRESENT)) {
                printf("elf_load_with_base: PT entry not present for 0x%lx\n",
                       page_vaddr);
                return -5;
            }

            uint64_t phys_page = pt[pt_idx] & PTE_ADDR_MASK;
            void *dst = (void *)(uintptr_t)(phys_page + page_off);

            memcpy(dst, (const uint8_t *)data + src_off, chunk);

            src_off       += chunk;
            dst_vaddr     += chunk;
            file_remaining -= chunk;
        }

        /* 将BSS部分清零 */
        if (memsz > filesz) {
            uint64_t bss_start = vaddr + filesz;
            uint64_t bss_end   = vaddr + memsz;

            uint64_t bss_page = bss_start & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t bss_off  = bss_start & (PAGE_SIZE - 1);
            size_t   first_chunk = PAGE_SIZE - (size_t)bss_off;

            uint64_t *pml4_tbl = (uint64_t *)(uintptr_t)proc->pml4;
            int pml4_idx = (int)(bss_page >> 39) & 0x1FF;
            uint64_t *pdpt_bss = (uint64_t *)(pml4_tbl[pml4_idx] & PTE_ADDR_MASK);
            int pdpt_idx = (int)(bss_page >> 30) & 0x1FF;
            uint64_t *pd_bss = (uint64_t *)(pdpt_bss[pdpt_idx] & PTE_ADDR_MASK);
            int pd_idx = (int)(bss_page >> 21) & 0x1FF;
            uint64_t *pt_bss = (uint64_t *)(pd_bss[pd_idx] & PTE_ADDR_MASK);
            int pt_idx = (int)(bss_page >> 12) & 0x1FF;
            uint64_t phys_bss = pt_bss[pt_idx] & PTE_ADDR_MASK;

            if (first_chunk > (size_t)(bss_end - bss_start))
                first_chunk = (size_t)(bss_end - bss_start);

            memset((void *)(uintptr_t)(phys_bss + bss_off), 0, first_chunk);
        }

        /* 跟踪最高地址，用于brk */
        if (vaddr + memsz > highest_addr)
            highest_addr = vaddr + memsz;
    }

    /* ---- 设置进程字段（仅首次加载时） ---- */
    if (!proc->stack_top) {
        /* 为PIE调整入口点 */
        uint64_t entry = ehdr->e_entry;
        if (ehdr->e_type == ET_DYN)
            entry += base_addr;

        proc->entry_point = entry;
        proc->brk         = (highest_addr + PAGE_SIZE - 1) &
                            ~(uint64_t)(PAGE_SIZE - 1);
        proc->brk        += 0x100000;  /* 1 MB 堆增长空间 */

        /* 映射用户栈 */
        uint64_t stack_top = 0x7FFFF0000000ULL;
        uint64_t stack_base = stack_top - PROC_STACK_SIZE;

        if (!map_user_range(proc, stack_base, PROC_STACK_SIZE,
                            PTE_USER | PTE_WRITABLE)) {
            printf("elf_load_with_base: failed to map user stack\n");
            return -6;
        }

        proc->stack_top = stack_top;
        proc->mmap_base = proc->brk + 0x10000000ULL;
    }

    return 0;
}

/* ========================================================================
 * elf_load_from_vfs – 从VFS路径加载ELF可执行文件
 *
 * 成功返回0，失败返回负值。
 * ======================================================================== */

int elf_load_from_vfs(process_t *proc, const char *path)
{
    /* 打开文件 */
    int fd = vfs_open(path, VFS_O_RDONLY, 0);
    if (fd < 0) {
        printf("elf_load_from_vfs: failed to open '%s'\n", path);
        return -1;
    }

    /* 定位到文件末尾以确定文件大小 */
    int64_t file_size = vfs_seek(fd, 0, VFS_SEEK_END);
    if (file_size <= 0) {
        printf("elf_load_from_vfs: failed to get size of '%s'\n", path);
        vfs_close(fd);
        return -2;
    }

    /* 定位回文件开头 */
    vfs_seek(fd, 0, VFS_SEEK_SET);

    /* 为整个文件分配内核缓冲区 */
    void *buf = kmalloc((size_t)file_size);
    if (!buf) {
        printf("elf_load_from_vfs: out of memory (%lld bytes)\n",
               (long long)file_size);
        vfs_close(fd);
        return -3;
    }

    /* 将文件读入缓冲区 */
    int bytes_read = vfs_read(fd, buf, (size_t)file_size);
    vfs_close(fd);

    if (bytes_read != (int)file_size) {
        printf("elf_load_from_vfs: short read (%d / %lld)\n",
               bytes_read, (long long)file_size);
        kfree(buf);
        return -4;
    }

    /* 加载ELF文件 */
    uint64_t entry = elf_load(proc, buf, (size_t)file_size);
    kfree(buf);

    if (entry == 0) {
        printf("elf_load_from_vfs: elf_load failed for '%s'\n", path);
        return -5;
    }

    return 0;
}
