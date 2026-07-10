#include "slab.h"
#include "memory.h"
#include "string.h"
#include "serial.h"

/* ---- Cache sizes: powers of two from 16 to 2048 ---- */
static const uint32_t cache_sizes[SLAB_NUM_CACHES] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

/* Global array of caches */
static slab_cache_t caches[SLAB_NUM_CACHES];

/* ---- Bitmap helpers ---- */

static inline void bitmap_set(uint8_t *bm, uint32_t idx)
{
    bm[idx / 8] |= (1 << (idx % 8));
}

static inline void bitmap_clear(uint8_t *bm, uint32_t idx)
{
    bm[idx / 8] &= ~(1 << (idx % 8));
}

static inline int bitmap_test(const uint8_t *bm, uint32_t idx)
{
    return (bm[idx / 8] >> (idx % 8)) & 1;
}

/* Number of bitmap bytes needed for n objects */
static inline uint32_t bitmap_bytes(uint32_t n)
{
    return (n + 7) / 8;
}

/* ---- Slab header size (fixed part, without bitmap) ---- */
#define SLAB_HDR_BASE sizeof(slab_t)

/* ---- List helpers ---- */

/* Remove a slab from a singly-linked list identified by its head pointer */
static void list_remove(slab_t **head, slab_t *target)
{
    slab_t **pp = head;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            target->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* Prepend a slab to a list */
static void list_prepend(slab_t **head, slab_t *slab)
{
    slab->next = *head;
    *head = slab;
}

/* ---- Create a new slab for a given cache ---- */

static slab_t *slab_create(slab_cache_t *cache)
{
    /*
     * Layout within the allocated page(s):
     *   [slab_t header][bitmap bytes][padding to obj_size alignment][objects...]
     *
     * We use 1 page for all cache sizes. For small objects this holds
     * many objects; for 2048-byte objects it still holds at least 1.
     */
    uint32_t obj_size = cache->obj_size;

    void *page = alloc_page();
    if (!page)
        return NULL;

    /* Determine how many objects fit.
     * Available space = PAGE_SIZE - SLAB_HDR_BASE
     * We need bitmap_bytes for the bitmap, and the rest for objects.
     * But bitmap size depends on object count, which depends on available
     * space, so we iterate. */
    uint32_t avail = PAGE_SIZE - SLAB_HDR_BASE;
    uint32_t obj_count = 0;
    uint32_t bm_size = 0;

    /* Iterative calculation: bitmap takes space too */
    for (int i = 0; i < 4; i++) {
        obj_count = avail / obj_size;
        if (obj_count == 0) {
            /* Even one object doesn't fit — shouldn't happen with our sizes */
            free_page(page);
            return NULL;
        }
        bm_size = bitmap_bytes(obj_count);
        /* Recalculate available: subtract bitmap from total avail */
        uint32_t new_avail = PAGE_SIZE - SLAB_HDR_BASE - bm_size;
        obj_count = new_avail / obj_size;
        if (obj_count == 0) {
            free_page(page);
            return NULL;
        }
        bm_size = bitmap_bytes(obj_count);
        avail = PAGE_SIZE - SLAB_HDR_BASE - bm_size;
    }

    /* Place the slab header at the start of the page */
    slab_t *slab = (slab_t *)page;
    slab->next      = NULL;
    slab->page      = page;
    slab->obj_size  = obj_size;
    slab->obj_count = obj_count;
    slab->obj_free  = obj_count;

    /* Bitmap starts right after the fixed header */
    uint8_t *bm = (uint8_t *)page + SLAB_HDR_BASE;

    /* Mark all objects as free (1 = free in our convention) */
    memset(bm, 0xFF, bm_size);

    return slab;
}

/* ---- Compute the address of object i within a slab ---- */

static void *slab_obj_addr(slab_t *slab, uint32_t idx)
{
    uint32_t bm_size = bitmap_bytes(slab->obj_count);
    /* Objects start after header + bitmap, aligned to obj_size */
    uintptr_t base = (uintptr_t)slab->page + SLAB_HDR_BASE + bm_size;
    /* Align up to obj_size boundary */
    base = (base + slab->obj_size - 1) & ~((uintptr_t)slab->obj_size - 1);
    return (void *)(base + (uintptr_t)idx * slab->obj_size);
}

/* ---- Find which slab a pointer belongs to within a cache ---- */

static slab_t *cache_find_slab(slab_cache_t *cache, void *ptr)
{
    /* Scan partial, full, and free lists */
    for (slab_t *s = cache->partial; s; s = s->next) {
        uintptr_t start = (uintptr_t)s->page;
        if ((uintptr_t)ptr >= start && (uintptr_t)ptr < start + PAGE_SIZE)
            return s;
    }
    for (slab_t *s = cache->full; s; s = s->next) {
        uintptr_t start = (uintptr_t)s->page;
        if ((uintptr_t)ptr >= start && (uintptr_t)ptr < start + PAGE_SIZE)
            return s;
    }
    for (slab_t *s = cache->free; s; s = s->next) {
        uintptr_t start = (uintptr_t)s->page;
        if ((uintptr_t)ptr >= start && (uintptr_t)ptr < start + PAGE_SIZE)
            return s;
    }
    return NULL;
}

/* ---- Find cache index for a given size ---- */

static int slab_cache_index(size_t size)
{
    for (int i = 0; i < SLAB_NUM_CACHES; i++) {
        if (size <= cache_sizes[i])
            return i;
    }
    return -1;  /* Too large for slab caches */
}

/* ---- Public API ---- */

void slab_init(void)
{
    for (int i = 0; i < SLAB_NUM_CACHES; i++) {
        caches[i].obj_size     = cache_sizes[i];
        caches[i].partial      = NULL;
        caches[i].full         = NULL;
        caches[i].free         = NULL;
        caches[i].total_allocs = 0;
        caches[i].total_frees  = 0;
    }
    serial_puts("[slab] initialized 8 caches (16..2048 bytes)\n");
}

void *slab_alloc(size_t size)
{
    if (size == 0)
        return NULL;

    int idx = slab_cache_index(size);
    if (idx < 0) {
        /* Too large for any slab cache — fall back to page allocator */
        size_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        void *p = alloc_pages(npages);
        return p;
    }

    slab_cache_t *cache = &caches[idx];

    /* 1. Try partial slabs first */
    if (cache->partial) {
        slab_t *slab = cache->partial;
        uint8_t *bm = (uint8_t *)slab->page + SLAB_HDR_BASE;

        /* Find first free object */
        for (uint32_t i = 0; i < slab->obj_count; i++) {
            if (bitmap_test(bm, i)) {
                bitmap_clear(bm, i);
                slab->obj_free--;
                cache->total_allocs++;

                /* If slab is now full, move it to the full list */
                if (slab->obj_free == 0) {
                    list_remove(&cache->partial, slab);
                    list_prepend(&cache->full, slab);
                }

                return slab_obj_addr(slab, i);
            }
        }
    }

    /* 2. Try free (spare) slab */
    if (cache->free) {
        slab_t *slab = cache->free;
        list_remove(&cache->free, slab);
        list_prepend(&cache->partial, slab);

        /* Allocate first object from it */
        uint8_t *bm = (uint8_t *)slab->page + SLAB_HDR_BASE;
        bitmap_clear(bm, 0);
        slab->obj_free--;
        cache->total_allocs++;
        return slab_obj_addr(slab, 0);
    }

    /* 3. Create a new slab */
    slab_t *slab = slab_create(cache);
    if (!slab) {
        serial_puts("[slab] failed to create new slab\n");
        return NULL;
    }

    list_prepend(&cache->partial, slab);

    /* Allocate first object */
    uint8_t *bm = (uint8_t *)slab->page + SLAB_HDR_BASE;
    bitmap_clear(bm, 0);
    slab->obj_free--;
    cache->total_allocs++;
    return slab_obj_addr(slab, 0);
}

void slab_free(void *ptr)
{
    if (!ptr)
        return;

    /* Search all caches for the slab that owns this pointer */
    for (int i = 0; i < SLAB_NUM_CACHES; i++) {
        slab_cache_t *cache = &caches[i];
        slab_t *slab = cache_find_slab(cache, ptr);
        if (!slab)
            continue;

        /* Determine object index from the pointer */
        uint32_t bm_size = bitmap_bytes(slab->obj_count);
        uintptr_t base = (uintptr_t)slab->page + SLAB_HDR_BASE + bm_size;
        base = (base + slab->obj_size - 1) & ~((uintptr_t)slab->obj_size - 1);

        uintptr_t offset = (uintptr_t)ptr - base;
        if (offset % slab->obj_size != 0) {
            serial_puts("[slab] unaligned pointer in slab_free\n");
            return;
        }
        uint32_t obj_idx = (uint32_t)(offset / slab->obj_size);
        if (obj_idx >= slab->obj_count) {
            serial_puts("[slab] pointer out of range in slab_free\n");
            return;
        }

        uint8_t *bm = (uint8_t *)slab->page + SLAB_HDR_BASE;
        if (!bitmap_test(bm, obj_idx)) {
            /* Already free — possible double free */
            bitmap_set(bm, obj_idx);
            slab->obj_free++;
            cache->total_frees++;
            serial_puts("[slab] warning: possible double free\n");
            return;
        }

        /* Mark object as free */
        bitmap_set(bm, obj_idx);
        slab->obj_free++;
        cache->total_frees++;

        /* Determine which list the slab is currently on and move as needed */
        int was_full = (slab->obj_free == 1);       /* just became non-full */
        int became_free = (slab->obj_free == slab->obj_count); /* fully free */

        if (became_free) {
            /* Remove from whichever list it's on */
            list_remove(&cache->partial, slab);
            list_remove(&cache->full, slab);

            if (cache->free) {
                /* Already have a spare free slab — release this one */
                free_page(slab->page);
            } else {
                /* Keep as spare */
                list_prepend(&cache->free, slab);
            }
        } else if (was_full) {
            /* Was on full list, now has a free slot — move to partial */
            list_remove(&cache->full, slab);
            list_prepend(&cache->partial, slab);
        }
        /* else: already on partial list, stays there */

        return;
    }

    /* If we get here, the pointer didn't belong to any slab cache.
     * It might have been a large allocation via alloc_pages().
     * We cannot determine the page count, so we free one page.
     * For large allocations, the caller should use the page allocator
     * API directly, or track the size themselves. */
    serial_puts("[slab] slab_free: pointer not found in any cache\n");
}

void *slab_realloc(void *ptr, size_t size)
{
    if (!ptr)
        return slab_alloc(size);

    if (size == 0) {
        slab_free(ptr);
        return NULL;
    }

    /* Find which cache owns this pointer */
    for (int i = 0; i < SLAB_NUM_CACHES; i++) {
        slab_cache_t *cache = &caches[i];
        slab_t *slab = cache_find_slab(cache, ptr);
        if (slab) {
            /* Current allocation size is slab->obj_size */
            if (size <= slab->obj_size) {
                /* Fits in current slab object */
                return ptr;
            }
            /* Need a larger allocation */
            void *new_ptr = slab_alloc(size);
            if (!new_ptr)
                return NULL;
            memcpy(new_ptr, ptr, slab->obj_size);
            slab_free(ptr);
            return new_ptr;
        }
    }

    /* Not in any slab cache — was a large page allocation.
     * We don't know the old size, so we can't copy.
     * Allocate new and return; caller must handle. */
    void *new_ptr = slab_alloc(size);
    if (!new_ptr)
        return NULL;
    /* Best effort: copy PAGE_SIZE bytes (minimum allocation unit).
     * The caller really should manage large allocations themselves. */
    memcpy(new_ptr, ptr, PAGE_SIZE);
    return new_ptr;
}

size_t slab_alloc_size(size_t size)
{
    int idx = slab_cache_index(size);
    if (idx < 0)
        return 0;
    return cache_sizes[idx];
}
