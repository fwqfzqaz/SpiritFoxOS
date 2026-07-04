#include "memory.h"

#define MAX_MEMORY_PAGES  131072  /* 512 MB worth of 4KB pages */

static uint8_t page_bitmap[MAX_MEMORY_PAGES / 8];
static uint64_t total_pages = 0;
static uint64_t first_usable_page = 0;

static void bitmap_set(uint64_t index)
{
    page_bitmap[index / 8] |= (1 << (index % 8));
}

static void bitmap_clear(uint64_t index)
{
    page_bitmap[index / 8] &= ~(1 << (index % 8));
}

static int bitmap_test(uint64_t index)
{
    return (page_bitmap[index / 8] >> (index % 8)) & 1;
}

void memory_init(BootInfo* info, uintptr_t kernel_end)
{
    /* Mark all pages as used initially */
    for (uint64_t i = 0; i < MAX_MEMORY_PAGES / 8; i++) {
        page_bitmap[i] = 0xFF;
    }

    /* Round kernel_end up to page boundary */
    uint64_t kernel_end_page = (kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;

    if (!info || !info->memory_map || info->memory_map_size == 0) {
        /* No memory map available (e.g. multiboot test mode)
         * Mark pages after kernel end up to 64MB as usable */
        total_pages = 0;
        first_usable_page = kernel_end_page;
        for (uint64_t i = kernel_end_page; i < 16384 && i < MAX_MEMORY_PAGES; i++) {
            bitmap_clear(i);
            total_pages++;
        }
        return;
    }

    uint64_t map_entries = info->memory_map_size / info->memory_map_descriptor_size;

    /* Walk the memory map and mark usable (type 7 = conventional) pages as free */
    for (uint64_t i = 0; i < map_entries; i++) {
        MemoryMapEntry* desc = (MemoryMapEntry*)(
            (uint64_t)info->memory_map + i * info->memory_map_descriptor_size
        );

        if (desc->type == 7) { /* Conventional memory */
            uint64_t start_page = desc->physical_start / PAGE_SIZE;
            uint64_t num_pages  = desc->number_of_pages;

            for (uint64_t p = 0; p < num_pages; p++) {
                uint64_t page_idx = start_page + p;
                if (page_idx < MAX_MEMORY_PAGES) {
                    bitmap_clear(page_idx);
                    if (total_pages == 0 && page_idx >= kernel_end_page) {
                        first_usable_page = page_idx;
                    }
                    total_pages++;
                }
            }
        }
    }

    /* Mark all kernel pages as used (0x100000 to kernel_end) */
    for (uint64_t i = 0; i < kernel_end_page && i < MAX_MEMORY_PAGES; i++) {
        bitmap_set(i);
    }

    /* Reserve the kernel stack area: 8MB-16MB (0x800000 - 0x1000000)
     * This prevents alloc_page() from giving out pages that the stack
     * might grow into, which would cause silent heap corruption. */
    for (uint64_t i = 0x800000 / PAGE_SIZE; i < 0x1000000 / PAGE_SIZE && i < MAX_MEMORY_PAGES; i++) {
        bitmap_set(i);
    }
}

void* alloc_page(void)
{
    /* Start searching from first_usable_page for a free page */
    for (uint64_t i = first_usable_page; i < MAX_MEMORY_PAGES; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            return (void*)(i * PAGE_SIZE);
        }
    }
    return (void*)0; /* Out of memory */
}

/* Allocate N contiguous physical pages, return physical address of first page */
void* alloc_pages(size_t count)
{
    if (count == 0) return (void*)0;
    if (count == 1) return alloc_page();

    /* Linear scan for count consecutive free pages */
    for (uint64_t i = first_usable_page; i + count <= MAX_MEMORY_PAGES; i++) {
        int ok = 1;
        for (size_t j = 0; j < count; j++) {
            if (bitmap_test(i + j)) {
                /* Skip past this occupied page */
                i += j;
                ok = 0;
                break;
            }
        }
        if (!ok) continue;

        /* Found count consecutive free pages */
        for (size_t j = 0; j < count; j++) {
            bitmap_set(i + j);
        }
        return (void*)(i * PAGE_SIZE);
    }
    return (void*)0; /* Out of memory */
}

void free_page(void* addr)
{
    uint64_t page_idx = (uint64_t)addr / PAGE_SIZE;
    if (page_idx < MAX_MEMORY_PAGES && bitmap_test(page_idx)) {
        bitmap_clear(page_idx);
    }
}

uint64_t pmm_total_pages(void)
{
    return total_pages;
}

uint64_t pmm_used_pages(void)
{
    /* Count used pages by scanning the bitmap */
    uint64_t count = 0;
    for (uint64_t i = 0; i < MAX_MEMORY_PAGES; i++) {
        if (bitmap_test(i)) {
            count++;
        }
    }
    return count;
}
