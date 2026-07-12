#include "memory.h"
#include "serial.h"
#include "hal.h"
#include "vga.h"

/* 最大可寻址物理内存：64 GB（16384个2MB页 = 4GB，
 * 但我们跟踪4KB页 = 16777216页 = 64GB） */
#define MAX_PHYS_ADDR   (64ULL * 1024 * 1024 * 1024)   /* 64 GB */
#define MAX_MEMORY_PAGES (MAX_PHYS_ADDR / PAGE_SIZE)    /* 16M 页 */

/* 动态位图：在初始化时根据实际内存分配 */
static uint8_t *page_bitmap = NULL;
static uint64_t bitmap_size = 0;       /* 位图大小（字节） */
static uint64_t max_pages = 0;         /* 基于实际内存的最大页数 */
static uint64_t total_pages = 0;
static uint64_t first_usable_page = 0;

/* 大页（2MB）跟踪 - 用于内核分配 */
#define HPAGE_SIZE      (2 * 1024 * 1024)   /* 2 MB */
#define HPAGE_SHIFT     21
#define PAGES_PER_HPAGE (HPAGE_SIZE / PAGE_SIZE)  /* 512 */

static void bitmap_set(uint64_t index)
{
    if (index >= max_pages) return;
    page_bitmap[index / 8] |= (1 << (index % 8));
}

static void bitmap_clear(uint64_t index)
{
    if (index >= max_pages) return;
    page_bitmap[index / 8] &= ~(1 << (index % 8));
}

static int bitmap_test(uint64_t index)
{
    if (index >= max_pages) return 1;  /* 超出范围 = 已使用 */
    return (page_bitmap[index / 8] >> (index % 8)) & 1;
}

void memory_init(BootInfo* info, uintptr_t kernel_end)
{
    /* 将kernel_end向上对齐到页面边界 */
    uint64_t kernel_end_page = (kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;

    if (!info || !info->memory_map || info->memory_map_size == 0) {
        /* 无可用内存映射 - 旧版回退
         * 将内核结束之后到64MB的页面标记为可用 */
        max_pages = 16384;  /* 64 MB */
        bitmap_size = max_pages / 8;
        page_bitmap = (uint8_t *)0x40000;  /* 使用固定的低位内存存放位图 */
        for (uint64_t i = 0; i < bitmap_size; i++)
            page_bitmap[i] = 0xFF;

        total_pages = 0;
        first_usable_page = kernel_end_page;
        for (uint64_t i = kernel_end_page; i < max_pages; i++) {
            bitmap_clear(i);
            total_pages++;
        }
        return;
    }

    /* 从内存映射确定最大物理地址 */
    uint64_t max_addr = 0;
    uint64_t map_entries = info->memory_map_size / info->memory_map_descriptor_size;

    for (uint64_t i = 0; i < map_entries; i++) {
        MemoryMapEntry* desc = (MemoryMapEntry*)(
            (uint64_t)info->memory_map + i * info->memory_map_descriptor_size
        );
        uint64_t entry_end = desc->physical_start + desc->number_of_pages * PAGE_SIZE;
        if (entry_end > max_addr)
            max_addr = entry_end;
    }

    /* 限制在MAX_PHYS_ADDR以内 */
    if (max_addr > MAX_PHYS_ADDR)
        max_addr = MAX_PHYS_ADDR;

    max_pages = max_addr / PAGE_SIZE;
    bitmap_size = (max_pages + 7) / 8;

    /* 分配位图 - 尝试在常规内存中找到合适的位置。
     * 对于UEFI，可以使用内存映射找到合适的区域。
     * 对于旧版启动，使用固定位置。 */
    if (info->boot_type == BOOT_TYPE_UEFI) {
        /* 在内存映射中搜索足够大的空闲区域来存放位图 */
        int found = 0;
        for (uint64_t i = 0; i < map_entries && !found; i++) {
            MemoryMapEntry* desc = (MemoryMapEntry*)(
                (uint64_t)info->memory_map + i * info->memory_map_descriptor_size
            );
            if (desc->type == 7) {  /* EfiConventionalMemory */
                if (desc->number_of_pages * PAGE_SIZE >= bitmap_size &&
                    desc->physical_start >= 0x100000) {  /* 1MB以上 */
                    page_bitmap = (uint8_t *)(uintptr_t)desc->physical_start;
                    found = 1;
                }
            }
        }
        if (!found) {
            /* 回退：如果位图足够小，使用640KB以下的低位内存 */
            if (bitmap_size <= 0x8000) {  /* 32KB或更小 */
                page_bitmap = (uint8_t *)0x500;
            } else {
                /* 严重错误：无法为位图分配空间 */
                serial_puts("[PMM] FATAL: Cannot allocate page bitmap!\n");
                while (1) __asm__ volatile("cli; hlt");
            }
        }
    } else {
        /* 旧版启动：使用低位内存或固定位置。
         * 注意：0x500-0x3FFF 被 loader 的 mmap 转换数据占用，
         * 0x8000-0x9FFF 被 AP trampoline 使用，
         * 0x9000-0x9FFF 被 BootInfo 使用。
         * 安全区域：0x4000-0x7FFF 或内核之后。 */
        if (bitmap_size <= 0x4000) {  /* 16KB 或更小（<= 128MB 内存） */
            page_bitmap = (uint8_t *)0x4000;
        } else {
            /* 对于较大的位图，使用内核之后的内存加偏移 */
            page_bitmap = (uint8_t *)(kernel_end & ~(uintptr_t)0xFFF);
        }
    }

    /* 初始时将所有页面标记为已使用 */
    for (uint64_t i = 0; i < bitmap_size; i++)
        page_bitmap[i] = 0xFF;

    /* 遍历内存映射，将可用页面标记为空闲 */
    for (uint64_t i = 0; i < map_entries; i++) {
        MemoryMapEntry* desc = (MemoryMapEntry*)(
            (uint64_t)info->memory_map + i * info->memory_map_descriptor_size
        );

        if (desc->type == 7) { /* 常规内存 */
            uint64_t start_page = desc->physical_start / PAGE_SIZE;
            uint64_t num_pages  = desc->number_of_pages;

            for (uint64_t p = 0; p < num_pages; p++) {
                uint64_t page_idx = start_page + p;
                if (page_idx < max_pages) {
                    bitmap_clear(page_idx);
                    if (total_pages == 0 && page_idx >= kernel_end_page) {
                        first_usable_page = page_idx;
                    }
                    total_pages++;
                }
            }
        }
    }

    /* 将所有内核页面标记为已使用 */
    for (uint64_t i = 0; i < kernel_end_page && i < max_pages; i++) {
        bitmap_set(i);
    }

    /* 保留内核栈区域：8MB-16MB */
    for (uint64_t i = 0x800000 / PAGE_SIZE; i < 0x1000000 / PAGE_SIZE && i < max_pages; i++) {
        bitmap_set(i);
    }

    /* 保留页位图本身占用的空间 */
    uint64_t bitmap_start_page = (uint64_t)(uintptr_t)page_bitmap / PAGE_SIZE;
    uint64_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < bitmap_pages; i++) {
        if (bitmap_start_page + i < max_pages)
            bitmap_set(bitmap_start_page + i);
    }

    if (info) {
        printf("[PMM] Boot type: %s, Total memory: %llu MB, Pages: %llu\n",
               info->boot_type == BOOT_TYPE_UEFI ? "UEFI" : "Legacy",
               (unsigned long long)(info->total_memory / (1024 * 1024)),
               (unsigned long long)total_pages);
    }
}

void* alloc_page(void)
{
    for (uint64_t i = first_usable_page; i < max_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            return (void*)(i * PAGE_SIZE);
        }
    }
    return (void*)0;
}

void* alloc_pages(size_t count)
{
    if (count == 0) return (void*)0;
    if (count == 1) return alloc_page();

    for (uint64_t i = first_usable_page; i + count <= max_pages; i++) {
        int ok = 1;
        for (size_t j = 0; j < count; j++) {
            if (bitmap_test(i + j)) {
                i += j;
                ok = 0;
                break;
            }
        }
        if (!ok) continue;

        for (size_t j = 0; j < count; j++) {
            bitmap_set(i + j);
        }
        return (void*)(i * PAGE_SIZE);
    }
    return (void*)0;
}

void free_page(void* addr)
{
    uint64_t page_idx = (uint64_t)addr / PAGE_SIZE;
    if (page_idx < max_pages && bitmap_test(page_idx)) {
        bitmap_clear(page_idx);
    }
}

/* 分配2MB大页（512个连续4KB页，2MB对齐） */
void* alloc_huge_page(void)
{
    /* 查找从2MB对齐地址开始的512个连续空闲页 */
    for (uint64_t i = first_usable_page; i + PAGES_PER_HPAGE <= max_pages; i++) {
        /* 检查对齐：页索引必须是512的倍数 */
        if ((i % PAGES_PER_HPAGE) != 0) {
            /* 跳到下一个2MB对齐位置 */
            i = ((i / PAGES_PER_HPAGE) + 1) * PAGES_PER_HPAGE - 1;
            continue;
        }

        int ok = 1;
        for (size_t j = 0; j < PAGES_PER_HPAGE; j++) {
            if (bitmap_test(i + j)) {
                i += j;
                ok = 0;
                break;
            }
        }
        if (!ok) continue;

        for (size_t j = 0; j < PAGES_PER_HPAGE; j++) {
            bitmap_set(i + j);
        }
        return (void*)(i * PAGE_SIZE);
    }
    return (void*)0;
}

void free_huge_page(void* addr)
{
    uint64_t page_idx = (uint64_t)addr / PAGE_SIZE;
    if ((page_idx % PAGES_PER_HPAGE) != 0) return;  /* 非大页对齐 */
    for (size_t j = 0; j < PAGES_PER_HPAGE; j++) {
        if (page_idx + j < max_pages)
            bitmap_clear(page_idx + j);
    }
}

uint64_t pmm_total_pages(void)
{
    return total_pages;
}

uint64_t pmm_used_pages(void)
{
    uint64_t count = 0;
    for (uint64_t i = 0; i < max_pages; i++) {
        if (bitmap_test(i)) {
            count++;
        }
    }
    return count;
}
