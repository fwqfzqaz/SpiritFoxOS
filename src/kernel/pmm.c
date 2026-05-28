#include "pmm.h"
#include "vga.h"
#include "../include/stddef.h"
#include <stdint.h>

static uint64_t *pmm_bitmap = NULL;
static uint64_t pmm_total_frames = 0;
static uint64_t pmm_free_frames = 0;
static uint64_t pmm_bitmap_size = 0; /* Number of qwords in bitmap */

static void pmm_mark_free(uint64_t base, uint64_t length) {
    uint64_t start = (base + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t end = (base + length) / PAGE_SIZE;
    for (uint64_t i = start; i < end && i < pmm_total_frames; i++) {
        uint64_t idx = i / BITS_PER_QWORD;
        uint64_t bit = i % BITS_PER_QWORD;
        if (pmm_bitmap[idx] & (1ULL << bit)) {
            pmm_bitmap[idx] &= ~(1ULL << bit);
            pmm_free_frames++;
        }
    }
}

static void pmm_mark_used(uint64_t base, uint64_t length) {
    uint64_t start = base / PAGE_SIZE;
    uint64_t end = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = start; i < end && i < pmm_total_frames; i++) {
        uint64_t idx = i / BITS_PER_QWORD;
        uint64_t bit = i % BITS_PER_QWORD;
        if (!(pmm_bitmap[idx] & (1ULL << bit))) {
            pmm_bitmap[idx] |= (1ULL << bit);
            pmm_free_frames--;
        }
    }
}

void pmm_init(struct multiboot2_tag_mmap *mmap, uint64_t kernel_start, uint64_t kernel_end) {
    /* Step 1: Find the highest available physical address */
    uint64_t max_addr = 0;
    uint64_t mmap_entries = (mmap->size - sizeof(*mmap)) / mmap->entry_size;

    for (uint64_t i = 0; i < mmap_entries; i++) {
        struct multiboot2_mmap_entry *entry = &mmap->entries[i];
        if (entry->type == MULTIBOOT2_MEMORY_AVAILABLE) {
            uint64_t end = entry->base_addr + entry->length;
            if (end > max_addr) max_addr = end;
        }
    }

    /* Step 2: Calculate bitmap size */
    pmm_total_frames = (max_addr + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_bitmap_size = (pmm_total_frames + BITS_PER_QWORD - 1) / BITS_PER_QWORD;
    uint64_t bitmap_bytes = pmm_bitmap_size * sizeof(uint64_t);

    /* Step 3: Find a region to place the bitmap itself */
    pmm_bitmap = NULL;
    for (uint64_t i = 0; i < mmap_entries; i++) {
        struct multiboot2_mmap_entry *entry = &mmap->entries[i];
        if (entry->type == MULTIBOOT2_MEMORY_AVAILABLE &&
            entry->length >= bitmap_bytes + PAGE_SIZE) {
            /* Place bitmap at the start of this region, page-aligned */
            uint64_t bitmap_addr = (entry->base_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            /* Make sure it doesn't overlap with kernel */
            if (bitmap_addr >= kernel_end || bitmap_addr + bitmap_bytes <= kernel_start) {
                pmm_bitmap = (uint64_t *)bitmap_addr;
                break;
            }
            /* Try after kernel */
            if (kernel_end + bitmap_bytes <= entry->base_addr + entry->length) {
                pmm_bitmap = (uint64_t *)((kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
                break;
            }
        }
    }

    if (!pmm_bitmap) {
        vga_puts("PMM: Failed to find space for bitmap!\n");
        return;
    }

    /* Step 4: Initialize bitmap - mark all as used */
    for (uint64_t i = 0; i < pmm_bitmap_size; i++) {
        pmm_bitmap[i] = 0xFFFFFFFFFFFFFFFFULL;
    }
    pmm_free_frames = 0;

    /* Step 5: Mark available memory as free */
    for (uint64_t i = 0; i < mmap_entries; i++) {
        struct multiboot2_mmap_entry *entry = &mmap->entries[i];
        if (entry->type == MULTIBOOT2_MEMORY_AVAILABLE) {
            pmm_mark_free(entry->base_addr, entry->length);
        }
    }

    /* Step 6: Mark kernel as used */
    pmm_mark_used(kernel_start, kernel_end - kernel_start);

    /* Step 7: Mark bitmap as used */
    pmm_mark_used((uint64_t)pmm_bitmap, bitmap_bytes);

    vga_printf("PMM: %u MB available (%u pages)\n",
               (uint32_t)(pmm_free_frames * PAGE_SIZE / (1024 * 1024)),
               (uint32_t)pmm_free_frames);
}

uint64_t pmm_alloc_page(void) {
    for (uint64_t i = 0; i < pmm_bitmap_size; i++) {
        if (pmm_bitmap[i] != 0xFFFFFFFFFFFFFFFFULL) {
            uint64_t bit = __builtin_ctzll(~pmm_bitmap[i]);
            pmm_bitmap[i] |= (1ULL << bit);
            pmm_free_frames--;
            return (i * BITS_PER_QWORD + bit) * PAGE_SIZE;
        }
    }
    return 0; /* Out of memory */
}

void pmm_free_page(uint64_t addr) {
    uint64_t frame = addr / PAGE_SIZE;
    uint64_t idx = frame / BITS_PER_QWORD;
    uint64_t bit = frame % BITS_PER_QWORD;
    if (idx < pmm_bitmap_size && (pmm_bitmap[idx] & (1ULL << bit))) {
        pmm_bitmap[idx] &= ~(1ULL << bit);
        pmm_free_frames++;
    }
}

uint64_t pmm_alloc_pages(uint64_t n) {
    if (n == 0) return 0;
    if (n == 1) return pmm_alloc_page();

    uint64_t consecutive = 0;
    uint64_t start_frame = 0;

    for (uint64_t i = 0; i < pmm_bitmap_size && consecutive < n; i++) {
        for (uint64_t bit = 0; bit < BITS_PER_QWORD && consecutive < n; bit++) {
            uint64_t frame = i * BITS_PER_QWORD + bit;
            if (frame >= pmm_total_frames) break;

            if (!(pmm_bitmap[i] & (1ULL << bit))) {
                if (consecutive == 0) start_frame = frame;
                consecutive++;
            } else {
                consecutive = 0;
            }
        }
    }

    if (consecutive < n) return 0;

    /* Mark pages as used */
    for (uint64_t f = start_frame; f < start_frame + n; f++) {
        uint64_t idx = f / BITS_PER_QWORD;
        uint64_t bit = f % BITS_PER_QWORD;
        pmm_bitmap[idx] |= (1ULL << bit);
    }
    pmm_free_frames -= n;
    return start_frame * PAGE_SIZE;
}

uint64_t pmm_free_count(void) {
    return pmm_free_frames;
}

uint64_t pmm_total_count(void) {
    return pmm_total_frames;
}
