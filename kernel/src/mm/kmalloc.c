#include "kmalloc.h"
#include "memory.h"
#include "string.h"
#include "serial.h"

/* 块头：共16字节 */
typedef struct block_header {
    uint32_t size;          /* 可用载荷大小（不包括头部） */
    uint32_t used;          /* 0 = 空闲, 1 = 已分配 */
    uint32_t arena_size;    /* 竞技场总大小（字节） */
    uint32_t arena_pgidx;   /* 竞技场起始页索引 (base / PAGE_SIZE) */
} block_header_t;

#define HEADER_SIZE   sizeof(block_header_t)  /* 16字节 */
#define ALIGNMENT     16
#define MIN_ALLOC     16

/* 向上对齐到ALIGNMENT边界 */
static size_t align_up(size_t val)
{
    return (val + ALIGNMENT - 1) & ~(size_t)(ALIGNMENT - 1);
}

/* 给定头部，返回用户载荷的指针 */
static void *header_to_payload(block_header_t *hdr)
{
    return (void *)((uint8_t *)hdr + HEADER_SIZE);
}

/* 给定用户载荷指针，返回头部 */
static block_header_t *payload_to_header(void *ptr)
{
    return (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
}

/* 给定头部，返回同一页面竞技场中的下一个头部，
   如果超出页面边界则返回NULL */
static block_header_t *next_block(block_header_t *hdr, size_t arena_size)
{
    uint8_t *next = (uint8_t *)hdr + HEADER_SIZE + hdr->size;
    uint8_t *arena_start = (uint8_t *)hdr - ((uintptr_t)hdr % PAGE_SIZE);
    if (next >= arena_start + arena_size)
        return NULL;
    return (block_header_t *)next;
}

/* ---- 全局状态 ---- */
static block_header_t *free_list;   /* 通过空闲块的载荷区域链接的单链表 */
static size_t total_used;           /* 用户当前分配的字节数 */
static size_t total_heap;           /* 堆竞技区总字节数 */

/* ---- 空闲链表辅助函数 ---- */

/* 我们将"下一个空闲"指针存储在空闲块的载荷区域中。
   空闲块至少为MIN_ALLOC = 16字节，足以存放一个指针。 */
static block_header_t **next_free_ptr(block_header_t *hdr)
{
    return (block_header_t **)header_to_payload(hdr);
}

/* 将空闲块插入空闲链表头部 */
static void free_list_insert(block_header_t *hdr)
{
    *next_free_ptr(hdr) = free_list;
    free_list = hdr;
}

/* 从空闲链表中移除指定块 */
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

/* ---- 竞技场管理 ---- */

/* 分配新的竞技场（一个或多个页面），并将其划分为一个大的空闲块。
 * 如果指定了min_size，则分配足够的页面以满足该分配需求。 */
static void add_arena_size(size_t min_size)
{
    /* 计算需要的页面数（至少1页） */
    size_t needed = min_size + HEADER_SIZE;
    size_t num_pages = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
    if (num_pages < 1) num_pages = 1;

    /* 通过串口调试（避免使用printf，因为它会调用kmalloc） */
    serial_puts("[kmalloc] add_arena np=");
    serial_put_dec((uint32_t)num_pages);
    serial_puts(" size=");
    serial_put_dec((uint32_t)min_size);
    serial_puts("\n");

    /* 分配连续页面 */
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

/* 分配单页竞技场（用于小型分配） */
static void add_arena(void)
{
    add_arena_size(0);
}

/* ---- 公共API ---- */

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

    /* 向上对齐并强制最小分配大小 */
    size = align_up(size);
    if (size < MIN_ALLOC)
        size = MIN_ALLOC;

    /* 在空闲链表中搜索合适的块（首次适配） */
    block_header_t **pp = &free_list;
    while (*pp) {
        block_header_t *hdr = *pp;
        if (hdr->size >= size) {
            /* 找到合适的块——从空闲链表中移除 */
            *pp = *next_free_ptr(hdr);

            /* 如果剩余空间足够分割为新块，则进行分割 */
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

    /* 未找到合适的块——为此请求分配新的竞技区 */
    add_arena_size(size);

    /* 重新搜索（不递归——仅再次搜索） */
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

    return NULL;  /* 分配失败 */
}

void *kcalloc(size_t num, size_t size)
{
    size_t total = num * size;
    /* 溢出检查 */
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

    /* 如果当前块已经足够大，直接返回 */
    if (hdr->size >= aligned)
        return ptr;

    /* 分配新块，复制数据，释放旧块 */
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

    /* 使用头部中的arena_pgidx确定竞技场边界。
     * 竞技场起始地址可直接从存储的页索引计算得出。 */
    size_t arena_size_val = hdr->arena_size;
    if (arena_size_val == 0) arena_size_val = PAGE_SIZE;  /* 回退 */
    uint8_t *arena_start = (uint8_t *)((uintptr_t)hdr->arena_pgidx * PAGE_SIZE);
    uint8_t *arena_end = arena_start + arena_size_val;

    /* 从竞技场起始位置遍历块链并合并 */
    block_header_t *cur = (block_header_t *)arena_start;
    while ((uint8_t *)cur < arena_end) {
        if (!cur->used) {
            /* 尝试与下一个块合并（如果下一个块也是空闲的） */
            block_header_t *nxt = (block_header_t *)((uint8_t *)cur + HEADER_SIZE + cur->size);
            while ((uint8_t *)nxt < arena_end && !nxt->used) {
                /* 在吸收之前先从空闲链表中移除nxt */
                free_list_remove(nxt);
                cur->size += HEADER_SIZE + nxt->size;
                nxt = (block_header_t *)((uint8_t *)cur + HEADER_SIZE + cur->size);
            }
        }
        cur = (block_header_t *)((uint8_t *)cur + HEADER_SIZE + cur->size);
        if (cur->size == 0)
            break;  /* 安全措施：避免在损坏块上无限循环 */
    }

    /* 合并后，hdr可能已被前面的块吸收。
       重新查找覆盖原始地址的空闲块。 */
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

    /* hdr未被吸收（前面没有空闲的相邻块） */
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
