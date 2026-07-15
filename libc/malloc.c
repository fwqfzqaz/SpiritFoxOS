/* SpiritFoxOS libc - Simple malloc implementation using brk/mmap */

#include <stddef.h>
#include <stdint.h>

extern int64_t sfk_syscall1(int64_t num, int64_t a1);
extern int64_t sfk_syscall6(int64_t num, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6);

#define SYS_brk   12
#define SYS_mmap   9
#define SYS_munmap 11

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED    ((void *)-1)

/* Block header: 16 bytes */
typedef struct {
    uint32_t size;       /* payload size */
    uint32_t used;       /* 0=free, 1=allocated */
    uint32_t prev_size;  /* size of previous block (for coalescing) */
    uint32_t magic;      /* 0xDEADBEEF for integrity check */
} malloc_header_t;

#define MALLOC_MAGIC    0xDEADBEEF
#define HEADER_SIZE     sizeof(malloc_header_t)
#define MALLOC_ALIGN    16

static void *heap_base = NULL;
static void *heap_end = NULL;
static void *heap_brk = NULL;

static void ensure_heap(void)
{
    if (!heap_base) {
        /* Use brk to get initial heap */
        heap_brk = (void *)sfk_syscall1(SYS_brk, 0);
        heap_base = heap_brk;
        /* Expand heap by 64KB initially */
        heap_brk = (void *)sfk_syscall1(SYS_brk, (int64_t)heap_base + 0x10000);
        heap_end = heap_brk;
    }
}

void *malloc(size_t size)
{
    if (size == 0) return NULL;
    
    /* Align size */
    size = (size + MALLOC_ALIGN - 1) & ~(size_t)(MALLOC_ALIGN - 1);
    if (size < 16) size = 16;
    
    ensure_heap();
    
    /* First-fit search */
    malloc_header_t *hdr = (malloc_header_t *)heap_base;
    while ((void *)hdr < heap_end) {
        if (!hdr->used && hdr->size >= size) {
            /* Found a free block */
            hdr->used = 1;
            return (void *)((uint8_t *)hdr + HEADER_SIZE);
        }
        hdr = (malloc_header_t *)((uint8_t *)hdr + HEADER_SIZE + hdr->size);
    }
    
    /* Need to expand heap */
    size_t needed = HEADER_SIZE + size;
    if ((uint8_t *)heap_end + needed > (uint8_t *)heap_brk) {
        /* Expand brk */
        size_t expand = (needed + 0x10000 - 1) & ~(size_t)0xFFFF;
        void *new_brk = (void *)sfk_syscall1(SYS_brk, (int64_t)heap_brk + expand);
        if (new_brk == heap_brk) return NULL;  /* OOM */
        heap_brk = new_brk;
    }
    
    /* Create new block at heap_end */
    hdr = (malloc_header_t *)heap_end;
    hdr->size = size;
    hdr->used = 1;
    hdr->prev_size = 0;
    hdr->magic = MALLOC_MAGIC;
    heap_end = (uint8_t *)hdr + HEADER_SIZE + size;
    
    return (void *)((uint8_t *)hdr + HEADER_SIZE);
}

void free(void *ptr)
{
    if (!ptr) return;
    
    malloc_header_t *hdr = (malloc_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (hdr->magic != MALLOC_MAGIC) return;
    hdr->used = 0;
    
    /* Coalesce with next block if free */
    malloc_header_t *next = (malloc_header_t *)((uint8_t *)hdr + HEADER_SIZE + hdr->size);
    if ((void *)next < heap_end && !next->used && next->magic == MALLOC_MAGIC) {
        hdr->size += HEADER_SIZE + next->size;
    }
}

void *calloc(size_t num, size_t size)
{
    size_t total = num * size;
    void *p = malloc(total);
    if (p) {
        for (size_t i = 0; i < total; i++)
            ((uint8_t *)p)[i] = 0;
    }
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    
    malloc_header_t *hdr = (malloc_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (hdr->size >= size) return ptr;
    
    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    
    /* Copy old data */
    size_t copy = hdr->size < size ? hdr->size : size;
    uint8_t *src = (uint8_t *)ptr;
    uint8_t *dst = (uint8_t *)new_ptr;
    for (size_t i = 0; i < copy; i++)
        dst[i] = src[i];
    
    free(ptr);
    return new_ptr;
}
