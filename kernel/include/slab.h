#ifndef SLAB_H
#define SLAB_H

#include <stdint.h>
#include <stddef.h>

#define SLAB_NUM_CACHES 8
#define SLAB_MIN_SIZE   16
#define SLAB_MAX_SIZE   2048

typedef struct slab {
    struct slab *next;        /* Next slab in cache */
    void        *page;        /* Page-aligned base of this slab */
    uint32_t     obj_size;    /* Object size in this slab */
    uint32_t     obj_count;   /* Total objects in this slab */
    uint32_t     obj_free;    /* Free objects count */
    uint8_t      bitmap[];    /* Flexible array: bitmap of free objects */
} slab_t;

typedef struct slab_cache {
    uint32_t    obj_size;     /* Size of objects in this cache */
    slab_t     *partial;      /* Slabs with some free objects */
    slab_t     *full;         /* Slabs with no free objects */
    slab_t     *free;         /* Completely free slabs (max 1 kept) */
    uint64_t    total_allocs; /* Stats */
    uint64_t    total_frees;
} slab_cache_t;

/* Initialize all slab caches */
void slab_init(void);

/* Allocate from appropriate cache */
void *slab_alloc(size_t size);

/* Free an allocation */
void slab_free(void *ptr);

/* Reallocate */
void *slab_realloc(void *ptr, size_t size);

/* Return the actual cache size for a given request (0 if too large) */
size_t slab_alloc_size(size_t size);

#endif /* SLAB_H */
