/*
 * SpiritFoxOS ELF64 Executable Loader
 *
 * Loads ELF64 executables into a process's address space, creating
 * the necessary page tables and mapping segments with proper
 * permissions for user-mode execution.
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
 * Page table traversal helpers
 * ======================================================================== */

/* Return the virtual address of the page-table entry at the given level
 * that maps `virt` within page table `table_phys`.
 * If the next-level table does not exist, allocate one (identity mapped).
 * `level` is 0=PML4, 1=PDPT, 2=PD, 3=PT (we walk from level 0 down). */

static uint64_t *walk_entry(uint64_t table_phys, uint64_t virt, int level)
{
    /* 9-bit index at each level */
    static const int shifts[4] = { 39, 30, 21, 12 };
    int idx = (int)((virt >> shifts[level]) & 0x1FF);

    uint64_t *table = (uint64_t *)(uintptr_t)table_phys;
    uint64_t entry  = table[idx];

    /* If the entry is present, check for huge page that needs splitting */
    if (entry & PTE_PRESENT) {
        /* At PD level (level 2), a 2MB huge page must be split into
         * 512 x 4KB pages before we can map a single 4KB page. */
        if (level == 2 && (entry & PTE_HUGE)) {
            /* Split the 2MB page into a PT with 512 x 4KB entries */
            void *new_pt = alloc_page();
            if (!new_pt)
                return NULL;
            memset(new_pt, 0, PAGE_SIZE);

            uint64_t huge_phys = entry & 0x000FFFFFFFE00000ULL; /* 2MB-aligned phys (bits 21+) */
            /* Extract flags: everything except the physical address and PS bit.
             * Add PTE_USER so user-mode code can access these pages
             * (the original kernel 2MB huge pages lack PTE_USER). */
            uint64_t huge_flags = entry & ~0x000FFFFFFFE00000ULL;
            huge_flags &= ~PTE_HUGE;  /* Clear Page Size bit */
            huge_flags |= PTE_USER;   /* Allow user-mode access */

            uint64_t *pt = (uint64_t *)new_pt;
            for (int i = 0; i < 512; i++) {
                pt[i] = (huge_phys + (uint64_t)i * PAGE_SIZE) | huge_flags;
            }

            /* Replace the PD entry: point to the new PT instead of the 2MB page */
            uint64_t pt_phys = (uint64_t)(uintptr_t)new_pt;
            table[idx] = pt_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

            /* Return the PD entry so the caller can walk further down */
            return &table[idx];
        }

        return &table[idx];
    }

    /* Need to allocate a new page table for the next level.
     * (At the last level – PT – we never call this to allocate,
     *  we just set the final PTE directly.) */
    void *new_page = alloc_page();
    if (!new_page)
        return NULL;

    memset(new_page, 0, PAGE_SIZE);

    /* The new page is identity mapped, so phys == virt */
    uint64_t new_phys = (uint64_t)(uintptr_t)new_page;

    /* Set the entry: present, writable, user-accessible */
    table[idx] = new_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    return &table[idx];
}

/* ========================================================================
 * map_user_page – map a single 4KB page in the process's address space
 * ======================================================================== */

static int map_user_page(process_t *proc, uint64_t virt, uint64_t phys,
                         uint64_t flags)
{
    uint64_t pml4_phys = proc->pml4;

    /* Walk PML4 -> PDPT -> PD, allocating tables as needed */
    uint64_t *pml4e = walk_entry(pml4_phys, virt, 0);
    if (!pml4e) return -1;

    uint64_t pdpt_phys = *pml4e & PTE_ADDR_MASK;
    uint64_t *pdpte = walk_entry(pdpt_phys, virt, 1);
    if (!pdpte) return -2;

    uint64_t pd_phys = *pdpte & PTE_ADDR_MASK;
    uint64_t *pde = walk_entry(pd_phys, virt, 2);
    if (!pde) return -3;

    /* At the PD level, the entry points to a PT */
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

    /* Set the final PTE */
    int pt_idx = (int)((virt >> 12) & 0x1FF);
    uint64_t *pt = (uint64_t *)(uintptr_t)pt_phys;
    pt[pt_idx] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;

    return 0;
}

/* ========================================================================
 * map_user_range – map a contiguous range of 4KB virtual pages,
 *                  allocating a physical page for each
 * ======================================================================== */

static uint64_t map_user_range(process_t *proc, uint64_t virt, size_t size,
                               uint64_t flags)
{
    uint64_t start = virt;
    uint64_t end   = (virt + size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        void *phys_page = alloc_page();
        if (!phys_page)
            return 0;  /* failure – not enough memory */

        memset(phys_page, 0, PAGE_SIZE);

        uint64_t phys = (uint64_t)(uintptr_t)phys_page;
        if (map_user_page(proc, v, phys, flags) != 0)
            return 0;
    }

    return start;
}

/* ========================================================================
 * create_user_pml4 – allocate a new PML4 with kernel mappings copied
 * ======================================================================== */

/* Deep-copy a page table: allocate a new page, copy all entries.
 * This is needed for the lower PML4 entries so that splitting
 * huge pages in the process's copy doesn't affect the kernel. */
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

    /* Copy ALL 512 PML4 entries from the kernel's page tables.
     * The kernel uses identity mapping in the lower half (PML4[0..255])
     * for code, data, heap, and MMIO.  Only copying the upper half
     * (PML4[256..511]) would lose all kernel mappings.
     *
     * For PML4 entries in the lower half (user space range), we
     * deep-copy the PDPT so that splitting huge pages in the
     * process's page tables doesn't affect the kernel's mappings.
     * For the upper half (kernel-only), shallow copy is safe. */
    for (int i = 0; i < 256; i++) {
        if (src[i] & PTE_PRESENT) {
            /* Deep copy the PDPT for this PML4 entry */
            uint64_t pdpt_phys = src[i] & PTE_ADDR_MASK;
            uint64_t new_pdpt = deep_copy_table(pdpt_phys);
            if (!new_pdpt) {
                /* Fallback: shallow copy */
                dst[i] = src[i];
            } else {
                /* Copy flags from original, but add USER | WRITABLE
                 * so user-mode code can traverse these page tables.
                 * The actual page permissions are enforced at the PT level. */
                dst[i] = new_pdpt | (src[i] & ~PTE_ADDR_MASK) | PTE_USER | PTE_WRITABLE;
            }

            /* Also deep-copy each PD within the PDPT, since
             * we may need to split 2MB huge pages into 4KB pages.
             * Add USER | WRITABLE to PD entries for the same reason. */
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

    /* Upper half: shallow copy (kernel-only, no splitting needed) */
    for (int i = 256; i < 512; i++)
        dst[i] = src[i];

    return (uint64_t)(uintptr_t)new_pml4;
}

/* ========================================================================
 * elf_load – load an ELF64 executable into a process's address space
 *
 * Returns the entry point address, or 0 on failure.
 * ======================================================================== */

uint64_t elf_load(process_t *proc, const void *data, size_t size)
{
    /* ---- Validate ELF header ---- */
    if (size < sizeof(Elf64_Ehdr))
        return 0;

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;

    /* Magic */
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3)
        return 0;

    /* Class */
    if (ehdr->e_ident[4] != ELFCLASS64)
        return 0;

    /* Data encoding */
    if (ehdr->e_ident[5] != ELFDATA2LSB)
        return 0;

    /* Machine */
    if (ehdr->e_machine != EM_X86_64)
        return 0;

    /* Type: executable or shared object (PIE) */
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        return 0;

    /* Must have program headers */
    if (ehdr->e_phnum == 0 || ehdr->e_phoff == 0)
        return 0;

    /* Sanity: program headers must fit in the data */
    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > size)
        return 0;

    /* ---- Create a new PML4 for the process ---- */
    uint64_t pml4 = create_user_pml4();
    if (!pml4)
        return 0;

    proc->pml4 = pml4;

    /* ---- Load PT_LOAD segments ---- */
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

        /* Ensure the segment data fits in the file */
        if (offset + filesz > size) {
            printf("elf_load: segment %u file data out of bounds\n", i);
            return 0;
        }

        /* Page-align the virtual address range */
        uint64_t seg_start = vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t seg_end   = (vaddr + memsz + PAGE_SIZE - 1) &
                             ~(uint64_t)(PAGE_SIZE - 1);
        size_t   seg_size  = (size_t)(seg_end - seg_start);

        /* Map the range, allocating physical pages */
        if (!map_user_range(proc, seg_start, seg_size, flags)) {
            printf("elf_load: failed to map segment %u at 0x%lx\n",
                   i, seg_start);
            return 0;
        }

        /* Copy file data into the mapped pages.
         * Since the kernel uses identity mapping and the user pages
         * are identity-mapped physical pages, we can write directly
         * through the physical addresses.  We walk the page tables
         * via the process's PML4 to find each physical page. */
        uint64_t file_remaining = filesz;
        uint64_t src_off       = offset;
        uint64_t dst_vaddr     = vaddr;

        while (file_remaining > 0) {
            uint64_t page_vaddr = dst_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t page_off   = dst_vaddr & (PAGE_SIZE - 1);
            size_t   chunk      = PAGE_SIZE - (size_t)page_off;
            if (chunk > file_remaining)
                chunk = (size_t)file_remaining;

            /* Walk the process's page tables to find the physical page */
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

        /* Zero-fill the BSS portion (memsz > filesz).
         * The physical pages were already zeroed by map_user_range,
         * so we only need to zero the partial page at the boundary. */
        if (memsz > filesz) {
            uint64_t bss_start = vaddr + filesz;
            uint64_t bss_end   = vaddr + memsz;

            /* The first page of BSS may overlap with the last file page */
            uint64_t bss_page = bss_start & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t bss_off  = bss_start & (PAGE_SIZE - 1);
            size_t   first_chunk = PAGE_SIZE - (size_t)bss_off;

            /* Walk page tables for the first partial page */
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

            /* Remaining full pages are already zeroed by alloc_page/memset
             * in map_user_range, so nothing more to do. */
        }

        /* Track highest address for brk */
        if (vaddr + memsz > highest_addr)
            highest_addr = vaddr + memsz;
    }

    /* ---- Set process fields ---- */
    proc->entry_point = ehdr->e_entry;
    proc->brk         = (highest_addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    proc->brk        += 0x100000;  /* 1 MB room for heap growth */

    /* ---- Map user stack: 4 MB at 0x7FFFF0000000 going down ---- */
    uint64_t stack_top = 0x7FFFF0000000ULL;
    uint64_t stack_base = stack_top - PROC_STACK_SIZE;  /* 4 MB below */

    if (!map_user_range(proc, stack_base, PROC_STACK_SIZE,
                        PTE_USER | PTE_WRITABLE)) {
        printf("elf_load: failed to map user stack\n");
        return 0;
    }

    proc->stack_top = stack_top;

    /* ---- Set mmap_base (for future mmap implementation) ---- */
    proc->mmap_base = proc->brk + 0x10000000ULL;  /* 256 MB after brk */

    return proc->entry_point;
}

/* ========================================================================
 * elf_load_with_base – load an ELF with a base address offset
 *
 * Like elf_load(), but maps all PT_LOAD segments at base_addr + p_vaddr
 * instead of just p_vaddr.  This is needed for PIE executables (ET_DYN)
 * and shared libraries (the dynamic linker).
 *
 * If proc->pml4 is already set, reuses it (for loading the interpreter
 * after the main binary).  Otherwise creates a new PML4.
 *
 * If proc->stack_top is 0 (first load), maps the user stack and sets
 * process fields.  Otherwise only loads PT_LOAD segments.
 *
 * Returns 0 on success, negative on error.
 * ======================================================================== */

int elf_load_with_base(process_t *proc, const void *data, size_t size,
                       uint64_t base_addr)
{
    /* ---- Validate ELF header ---- */
    if (size < sizeof(Elf64_Ehdr))
        return -1;

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;

    /* Magic */
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3)
        return -1;

    /* Class */
    if (ehdr->e_ident[4] != ELFCLASS64)
        return -1;

    /* Data encoding */
    if (ehdr->e_ident[5] != ELFDATA2LSB)
        return -1;

    /* Machine */
    if (ehdr->e_machine != EM_X86_64)
        return -1;

    /* Type: executable or shared object (PIE) */
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        return -1;

    /* Must have program headers */
    if (ehdr->e_phnum == 0 || ehdr->e_phoff == 0)
        return -1;

    /* Sanity: program headers must fit in the data */
    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > size)
        return -1;

    /* ---- Create or reuse PML4 ---- */
    if (!proc->pml4) {
        uint64_t pml4 = create_user_pml4();
        if (!pml4)
            return -2;
        proc->pml4 = pml4;
    }

    /* ---- Load PT_LOAD segments ---- */
    uint64_t highest_addr = 0;
    const Elf64_Phdr *phdr = (const Elf64_Phdr *)
        ((const uint8_t *)data + ehdr->e_phoff);

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        /* For ET_DYN, add base_addr; for ET_EXEC, base_addr should be 0 */
        uint64_t vaddr  = phdr[i].p_vaddr + base_addr;
        uint64_t filesz = phdr[i].p_filesz;
        uint64_t memsz  = phdr[i].p_memsz;
        uint64_t offset = phdr[i].p_offset;
        uint64_t flags  = PTE_USER;

        if (phdr[i].p_flags & PF_W)
            flags |= PTE_WRITABLE;

        /* Ensure the segment data fits in the file */
        if (offset + filesz > size) {
            printf("elf_load_with_base: segment %u file data out of bounds\n", i);
            return -3;
        }

        /* Page-align the virtual address range */
        uint64_t seg_start = vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t seg_end   = (vaddr + memsz + PAGE_SIZE - 1) &
                             ~(uint64_t)(PAGE_SIZE - 1);
        size_t   seg_size  = (size_t)(seg_end - seg_start);

        /* Map the range, allocating physical pages */
        if (!map_user_range(proc, seg_start, seg_size, flags)) {
            printf("elf_load_with_base: failed to map segment %u at 0x%lx\n",
                   i, seg_start);
            return -4;
        }

        /* Copy file data into the mapped pages */
        uint64_t file_remaining = filesz;
        uint64_t src_off       = offset;
        uint64_t dst_vaddr     = vaddr;

        while (file_remaining > 0) {
            uint64_t page_vaddr = dst_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t page_off   = dst_vaddr & (PAGE_SIZE - 1);
            size_t   chunk      = PAGE_SIZE - (size_t)page_off;
            if (chunk > file_remaining)
                chunk = (size_t)file_remaining;

            /* Walk the process's page tables to find the physical page */
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

        /* Zero-fill BSS portion */
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

        /* Track highest address for brk */
        if (vaddr + memsz > highest_addr)
            highest_addr = vaddr + memsz;
    }

    /* ---- Set process fields (only for first load) ---- */
    if (!proc->stack_top) {
        /* Adjust entry point for PIE */
        uint64_t entry = ehdr->e_entry;
        if (ehdr->e_type == ET_DYN)
            entry += base_addr;

        proc->entry_point = entry;
        proc->brk         = (highest_addr + PAGE_SIZE - 1) &
                            ~(uint64_t)(PAGE_SIZE - 1);
        proc->brk        += 0x100000;  /* 1 MB room for heap growth */

        /* Map user stack */
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
 * elf_load_from_vfs – load an ELF executable from a VFS path
 *
 * Returns 0 on success, negative on error.
 * ======================================================================== */

int elf_load_from_vfs(process_t *proc, const char *path)
{
    /* Open the file */
    int fd = vfs_open(path, VFS_O_RDONLY, 0);
    if (fd < 0) {
        printf("elf_load_from_vfs: failed to open '%s'\n", path);
        return -1;
    }

    /* Seek to end to determine file size */
    int64_t file_size = vfs_seek(fd, 0, VFS_SEEK_END);
    if (file_size <= 0) {
        printf("elf_load_from_vfs: failed to get size of '%s'\n", path);
        vfs_close(fd);
        return -2;
    }

    /* Seek back to start */
    vfs_seek(fd, 0, VFS_SEEK_SET);

    /* Allocate a kernel buffer for the entire file */
    void *buf = kmalloc((size_t)file_size);
    if (!buf) {
        printf("elf_load_from_vfs: out of memory (%lld bytes)\n",
               (long long)file_size);
        vfs_close(fd);
        return -3;
    }

    /* Read the file into the buffer */
    int bytes_read = vfs_read(fd, buf, (size_t)file_size);
    vfs_close(fd);

    if (bytes_read != (int)file_size) {
        printf("elf_load_from_vfs: short read (%d / %lld)\n",
               bytes_read, (long long)file_size);
        kfree(buf);
        return -4;
    }

    /* Load the ELF */
    uint64_t entry = elf_load(proc, buf, (size_t)file_size);
    kfree(buf);

    if (entry == 0) {
        printf("elf_load_from_vfs: elf_load failed for '%s'\n", path);
        return -5;
    }

    return 0;
}
