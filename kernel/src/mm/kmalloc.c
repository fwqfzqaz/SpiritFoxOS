#include "kmalloc.h"
#include "memory.h"
#include "string.h"
#include "serial.h"

/* Block header: 16 bytes total */
typedef struct block_header {
    uint32_t size;          /* usable payload size (not including header) */
    uint32_t used;          /* 0 = free, 1 = allocated */
    uint32_t arena_size;    /* total arena size in bytes */
    uint32_t arena_pgidx;   /* page index of arena start (base / PAGE_SIZE) */
} block_header_t;

#define HEADER_SIZE   sizeof(block_header_t)  /* 16 bytes */
#define ALIGNMENT     16
#define MIN_ALLOC     16

/* Align up to ALIGNMENT boundary */
static size_t align_up(size_t val)
{
    return (val + ALIGNMENT - 1) & ~(size_t)(ALIGNMENT - 1);
}

/* Given a header, return the pointer to the user payload */
static void *header_to_payload(block_header_t *hdr)
{
    return (void *)((uint8_t *)hdr + HEADER_SIZE);
}

/* Given a user payload pointer, return the header */
static block_header_t *payload_to_header(void *ptr)
{
    return (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
}

/* Given a header, return the next header in the same page arena,
   or NULL if it would exceed the page boundary */
static block_header_t *next_block(block_header_t *hdr, size_t arena_size)
{
    uint8_t *next = (uint8_t *)hdr + HEADER_SIZE + hdr->size;
    uint8_t *arena_start = (uint8_t *)hdr - ((uintptr_t)hdr % PAGE_SIZE);
    if (next >= arena_start + arena_size)
        return NULL;
    return (block_header_t *)next;
}

/* ---- Global state ---- */
static block_header_t *free_list;   /* singly-linked via payload area of free blocks */
static size_t total_used;           /* bytes currently allocated by users */
static size_t total_heap;           /* total bytes in heap arenas */

/* ---- Free list helpers ---- */

/* We store the "next free" pointer in the payload of free blocks.
   A free block is at least MIN_ALLOC = 16 bytes, which fits a pointer. */
static block_header_t **next_free_ptr(block_header_t *hdr)
{
    return (block_header_t **)header_to_payload(hdr);
}

/* Insert a free block at the head of the free list */
static void free_list_insert(block_header_t *hdr)
{
    *next_free_ptr(hdr) = free_list;
    free_list = hdr;
}

/* Remove a specific block from the free list */
static void free_list_remove(block_header_t *hdr)
{
    block_header_t **pp = &free_list;
    while (*pp) {
        if (*pp == hdr) {
            *pp = *next_free_ptr(hdr);
            return;
        }
        pp = next_free_ptr(*pp);
    }
}

/* ---- Arena management ---- */

/* Allocate a new arena (one or more pages) and carve it into one large free block.
 * If min_size is specified, allocate enough pages to satisfy that allocation. */
static void add_arena_size(size_t min_size)
{
    /* Calculate how many pages we need (at least 1) */
    size_t needed = min_size + HEADER_SIZE;
    size_t num_pages = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
    if (num_pages < 1) num_pages = 1;

    /* Debug via serial port (avoid printf which uses kmalloc) */
    serial_puts("[kmalloc] add_arena np=");
    serial_put_dec((uint32_t)num_pages);
    serial_puts(" size=");
    serial_put_dec((uint32_t)min_size);
    serial_puts("\n");

    /* Allocate contiguous pages */
    void *base = alloc_pages(num_pages);
    if (!base) {
        serial_puts("[kmalloc] alloc_pages FAILED!\n");
        return;
    }

    size_t arena_size = num_pages * PAGE_SIZE;
    block_header_t *hdr = (block_header_t *)base;
    hdr->size = arena_size - HEADER_SIZE;
    hdr->used = 0;
    hdr->arena_size = (uint32_t)arena_size;
    hdr->arena_pgidx = (uint32_t)((uintptr_t)base / PAGE_SIZE);

    free_list_insert(hdr);
    total_heap += arena_size;
}

/* Allocate a single-page arena (for small allocations) */
static void add_arena(void)
{
    add_arena_size(0);
}

/* ---- Public API ---- */

void kmalloc_init(void)
{
    free_list = NULL;
    total_used = 0;
    total_heap = 0;
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    /* Round up to alignment and enforce minimum */
    size = align_up(size);
    if (size < MIN_ALLOC)
        size = MIN_ALLOC;

    /* Search the free list for a fit (first-fit) */
    block_header_t **pp = &free_list;
    while (*pp) {
        block_header_t *hdr = *pp;
        if (hdr->size >= size) {
            /* Found a fit — remove from free list */
            *pp = *next_free_ptr(hdr);

            /* Split if remainder is large enough for a new block */
            if (hdr->size >= size + HEADER_SIZE + MIN_ALLOC) {
                block_header_t *rest = (block_header_t *)((uint8_t *)hdr + HEADER_SIZE + size);
                rest->size = hdr->size - size - HEADER_SIZE;
                rest->used = 0;
                rest->arena_size = hdr->arena_size;
                rest->arena_pgidx = hdr->arena_pgidx;
                free_list_insert(rest);
                hdr->size = size;
            }

            hdr->used = 1;
            total_used += hdr->size;
            return header_to_payload(hdr);
        }
        pp = next_free_ptr(hdr);
    }

    /* No fit found — allocate a new arena large enough for this request */
    add_arena_size(size);

    /* Retry the search (don't recurse — just search again) */
    block_header_t **pp2 = &free_list;
    while (*pp2) {
        block_header_t *hdr = *pp2;
        if (hdr->size >= size) {
            *pp2 = *next_free_ptr(hdr);
            if (hdr->size >= size + HEADER_SIZE + MIN_ALLOC) {
                block_header_t *rest = (block_header_t *)((uint8_t *)hdr + HEADER_SIZE + size);
                rest->size = hdr->size - size - HEADER_SIZE;
                rest->used = 0;
                rest->arena_size = hdr->arena_size;
                rest->arena_pgidx = hdr->arena_pgidx;
                free_list_insert(rest);
                hdr->size = size;
            }
            hdr->used = 1;
            total_used += hdr->size;
            return header_to_payload(hdr);
        }
        pp2 = next_free_ptr(hdr);
    }

    return NULL;  /* allocation failed */
}

void *kcalloc(size_t num, size_t size)
{
    size_t total = num * size;
    /* Overflow check */
    if (num != 0 && total / num != size)
        return NULL;

    void *ptr = kmalloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr)
        return kmalloc(size);

    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    block_header_t *hdr = payload_to_header(ptr);

    size_t aligned = align_up(size);
    if (aligned < MIN_ALLOC)
        aligned = MIN_ALLOC;

    /* If current block is already large enough, just return */
    if (hdr->size >= aligned)
        return ptr;

    /* Allocate new block, copy, free old */
    void *new_ptr = kmalloc(size);
    if (!new_ptr)
        return NULL;

    memcpy(new_ptr, ptr, hdr->size);
    kfree(ptr);
    return new_ptr;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    block_header_t *hdr = payload_to_header(ptr);

    if (hdr->used != 1) {
        serial_puts("[kmalloc] double free or corruption!\n");
        return;
    }

    total_used -= hdr->size;
    hdr->used = 0;

    /* Determine arena boundaries using arena_pgidx from the header.
     * The arena start is directly computed from the stored page index. */
    size_t arena_size_val = hdr->arena_size;
    if (arena_size_val == 0) arena_size_val = PAGE_SIZE;  /* fallback */
    uint8_t *arena_start = (uint8_t *)((uintptr_t)hdr->arena_pgidx * PAGE_SIZE);
    uint8_t *arena_end = arena_start + arena_size_val;

    /* Walk the block chain from arena start and coalesce */
    block_header_t *cur = (block_header_t *)arena_start;
    while ((uint8_t *)cur < arena_end) {
        if (!cur->used) {
            /* Try to merge with the next block if it's also free */
            block_header_t *nxt = (block_header_t *)((uint8_t *)cur + HEADER_SIZE + cur->size);
            while ((uint8_t *)nxt < arena_end && !nxt->used) {
                /* Remove nxt from free list before absorbing */
                free_list_remove(nxt);
                cur->size += HEADER_SIZE + nxt->size;
                nxt = (block_header_t *)((uint8_t *)cur + HEADER_SIZE + cur->size);
            }
        }
        cur = (block_header_t *)((uint8_t *)cur + HEADER_SIZE + cur->size);
        if (cur->size == 0)
            break;  /* safety: avoid infinite loop on corrupt block */
    }

    /* After coalescing, hdr may have been absorbed into an earlier block.
       Re-find the free block that covers our original address. */
    cur = (block_header_t *)arena_start;
    while ((uint8_t *)cur < arena_end) {
        if (!cur->used) {
            uint8_t *block_end = (uint8_t *)cur + HEADER_SIZE + cur->size;
            if ((uint8_t *)hdr >= (uint8_t *)cur && (uint8_t *)hdr < block_end) {
                free_list_insert(cur);
                return;
            }
        }
        cur = (block_header_t *)((uint8_t *)cur + HEADER_SIZE + cur->size);
        if (cur->size == 0)
            break;
    }

    /* hdr was not absorbed (no earlier neighbor was free) */
    free_list_insert(hdr);
}

size_t kmalloc_used(void)
{
    return total_used;
}

size_t kmalloc_total(void)
{
    return total_heap;
}
