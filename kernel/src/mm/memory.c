#include "memory.h"
#include "serial.h"
#include "hal.h"
#include "vga.h"

/* Maximum addressable physical memory: 64 GB (16384 pages of 2MB = 4GB,
 * but we track 4KB pages = 16777216 pages for 64GB) */
#define MAX_PHYS_ADDR   (64ULL * 1024 * 1024 * 1024)   /* 64 GB */
#define MAX_MEMORY_PAGES (MAX_PHYS_ADDR / PAGE_SIZE)    /* 16M pages */

/* Dynamic bitmap: allocated at init time based on actual memory */
static uint8_t *page_bitmap = NULL;
static uint64_t bitmap_size = 0;       /* Size of bitmap in bytes */
static uint64_t max_pages = 0;         /* Actual max pages based on memory */
static uint64_t total_pages = 0;
static uint64_t first_usable_page = 0;

/* Huge page (2MB) tracking - for kernel allocations */
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
    if (index >= max_pages) return 1;  /* Out of range = used */
    return (page_bitmap[index / 8] >> (index % 8)) & 1;
}

void memory_init(BootInfo* info, uintptr_t kernel_end)
{
    /* Round kernel_end up to page boundary */
    uint64_t kernel_end_page = (kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;

    if (!info || !info->memory_map || info->memory_map_size == 0) {
        /* No memory map available - legacy fallback
         * Mark pages after kernel end up to 64MB as usable */
        max_pages = 16384;  /* 64 MB */
        bitmap_size = max_pages / 8;
        page_bitmap = (uint8_t *)0x40000;  /* Use fixed low memory for bitmap */
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

    /* Determine the maximum physical address from memory map */
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

    /* Cap at MAX_PHYS_ADDR */
    if (max_addr > MAX_PHYS_ADDR)
        max_addr = MAX_PHYS_ADDR;

    max_pages = max_addr / PAGE_SIZE;
    bitmap_size = (max_pages + 7) / 8;

    /* Allocate bitmap - try to find a location in conventional memory.
     * For UEFI, we can use the memory map to find a suitable region.
     * For legacy boot, use a fixed location. */
    if (info->boot_type == BOOT_TYPE_UEFI) {
        /* Search memory map for a free region large enough for the bitmap */
        int found = 0;
        for (uint64_t i = 0; i < map_entries && !found; i++) {
            MemoryMapEntry* desc = (MemoryMapEntry*)(
                (uint64_t)info->memory_map + i * info->memory_map_descriptor_size
            );
            if (desc->type == 7) {  /* EfiConventionalMemory */
                if (desc->number_of_pages * PAGE_SIZE >= bitmap_size &&
                    desc->physical_start >= 0x100000) {  /* Above 1MB */
                    page_bitmap = (uint8_t *)(uintptr_t)desc->physical_start;
                    found = 1;
                }
            }
        }
        if (!found) {
            /* Fallback: use low memory below 640KB if bitmap is small enough */
            if (bitmap_size <= 0x8000) {  /* 32KB or less */
                page_bitmap = (uint8_t *)0x500;
            } else {
                /* Critical: can't find space for bitmap */
                serial_puts("[PMM] FATAL: Cannot allocate page bitmap!\n");
                while (1) __asm__ volatile("cli; hlt");
            }
        }
    } else {
        /* Legacy boot: use low memory or fixed location */
        if (bitmap_size <= 0x8000) {
            page_bitmap = (uint8_t *)0x500;
        } else {
            /* For larger bitmaps, use memory after kernel + some offset */
            page_bitmap = (uint8_t *)(kernel_end & ~(uintptr_t)0xFFF);
        }
    }

    /* Mark all pages as used initially */
    for (uint64_t i = 0; i < bitmap_size; i++)
        page_bitmap[i] = 0xFF;

    /* Walk the memory map and mark usable pages as free */
    for (uint64_t i = 0; i < map_entries; i++) {
        MemoryMapEntry* desc = (MemoryMapEntry*)(
            (uint64_t)info->memory_map + i * info->memory_map_descriptor_size
        );

        if (desc->type == 7) { /* Conventional memory */
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

    /* Mark all kernel pages as used */
    for (uint64_t i = 0; i < kernel_end_page && i < max_pages; i++) {
        bitmap_set(i);
    }

    /* Reserve the kernel stack area: 8MB-16MB */
    for (uint64_t i = 0x800000 / PAGE_SIZE; i < 0x1000000 / PAGE_SIZE && i < max_pages; i++) {
        bitmap_set(i);
    }

    /* Reserve the page bitmap itself */
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

/* Allocate a 2MB huge page (512 contiguous 4KB pages, 2MB aligned) */
void* alloc_huge_page(void)
{
    /* Find 512 contiguous free pages starting at a 2MB-aligned address */
    for (uint64_t i = first_usable_page; i + PAGES_PER_HPAGE <= max_pages; i++) {
        /* Check alignment: page index must be multiple of 512 */
        if ((i % PAGES_PER_HPAGE) != 0) {
            /* Skip to next 2MB alignment */
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
    if ((page_idx % PAGES_PER_HPAGE) != 0) return;  /* Not huge page aligned */
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
