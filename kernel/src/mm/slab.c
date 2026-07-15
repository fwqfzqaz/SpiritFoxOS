#include "slab.h"
#include "memory.h"
#include "string.h"
#include "serial.h"

/* ---- 缓存大小：从16到2048的2的幂 ---- */
static const uint32_t cache_sizes[SLAB_NUM_CACHES] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

/* 全局缓存数组 */
static slab_cache_t caches[SLAB_NUM_CACHES];

/* ---- 位图辅助函数 ---- */

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

/* n个对象需要的位图字节数 */
static inline uint32_t bitmap_bytes(uint32_t n)
{
    return (n + 7) / 8;
}

/* ---- Slab头部大小（固定部分，不含位图） ---- */
#define SLAB_HDR_BASE sizeof(slab_t)

/* ---- 链表辅助函数 ---- */

/* 从由头指针标识的单链表中移除一个slab */
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

/* 将slab插入链表头部 */
static void list_prepend(slab_t **head, slab_t *slab)
{
    slab->next = *head;
    *head = slab;
}

/* ---- 为指定缓存创建新的slab ---- */

static slab_t *slab_create(slab_cache_t *cache)
{
    /*
     * 在分配的页面内的布局：
     *   [slab_t头部][位图字节][填充至obj_size对齐][对象...]
     *
     * 所有缓存大小都使用1页。对于小对象，可以容纳
     * 很多对象；对于2048字节的对象，至少也能容纳1个。
     */
    uint32_t obj_size = cache->obj_size;

    void *page = alloc_page();
    if (!page)
        return NULL;

    /* 确定能容纳多少个对象。
     * 可用空间 = PAGE_SIZE - SLAB_HDR_BASE
     * 需要bitmap_bytes字节存放位图，其余用于对象。
     * 但位图大小取决于对象数量，对象数量又取决于可用
     * 空间，所以需要迭代计算。 */
    uint32_t avail = PAGE_SIZE - SLAB_HDR_BASE;
    uint32_t obj_count = 0;
    uint32_t bm_size = 0;

    /* 迭代计算：位图也占空间 */
    for (int i = 0; i < 4; i++) {
        obj_count = avail / obj_size;
        if (obj_count == 0) {
            /* 连一个对象都放不下——在我们的尺寸下不应发生 */
            free_page(page);
            return NULL;
        }
        bm_size = bitmap_bytes(obj_count);
        /* 重新计算可用空间：从总可用空间中减去位图 */
        uint32_t new_avail = PAGE_SIZE - SLAB_HDR_BASE - bm_size;
        obj_count = new_avail / obj_size;
        if (obj_count == 0) {
            free_page(page);
            return NULL;
        }
        bm_size = bitmap_bytes(obj_count);
        avail = PAGE_SIZE - SLAB_HDR_BASE - bm_size;
    }

    /* 将slab头部放在页面起始位置 */
    slab_t *slab = (slab_t *)page;
    slab->next      = NULL;
    slab->page      = page;
    slab->obj_size  = obj_size;
    slab->obj_count = obj_count;
    slab->obj_free  = obj_count;

    /* 位图紧跟在固定头部之后 */
    uint8_t *bm = (uint8_t *)page + SLAB_HDR_BASE;

    /* 将所有对象标记为空闲（1 = 空闲，按我们的约定） */
    memset(bm, 0xFF, bm_size);

    return slab;
}

/* ---- 计算slab中第i个对象的地址 ---- */

static void *slab_obj_addr(slab_t *slab, uint32_t idx)
{
    uint32_t bm_size = bitmap_bytes(slab->obj_count);
    /* 对象在头部 + 位图之后开始，对齐到obj_size */
    uintptr_t base = (uintptr_t)slab->page + SLAB_HDR_BASE + bm_size;
    /* 向上对齐到obj_size边界 */
    base = (base + slab->obj_size - 1) & ~((uintptr_t)slab->obj_size - 1);
    return (void *)(base + (uintptr_t)idx * slab->obj_size);
}

/* ---- 查找指针所属的slab ---- */

static slab_t *cache_find_slab(slab_cache_t *cache, void *ptr)
{
    /* 扫描partial、full和free链表 */
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

/* ---- 查找给定大小对应的缓存索引 ---- */

static int slab_cache_index(size_t size)
{
    for (int i = 0; i < SLAB_NUM_CACHES; i++) {
        if (size <= cache_sizes[i])
            return i;
    }
    return -1;  /* 对slab缓存来说太大 */
}

/* ---- 公共API ---- */

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
        /* 对任何slab缓存来说都太大——回退到页面分配器 */
        size_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        void *p = alloc_pages(npages);
        return p;
    }

    slab_cache_t *cache = &caches[idx];

    /* 1. 首先尝试partial slab */
    if (cache->partial) {
        slab_t *slab = cache->partial;
        uint8_t *bm = (uint8_t *)slab->page + SLAB_HDR_BASE;

        /* 查找第一个空闲对象 */
        for (uint32_t i = 0; i < slab->obj_count; i++) {
            if (bitmap_test(bm, i)) {
                bitmap_clear(bm, i);
                slab->obj_free--;
                cache->total_allocs++;

                /* 如果slab已满，将其移到full链表 */
                if (slab->obj_free == 0) {
                    list_remove(&cache->partial, slab);
                    list_prepend(&cache->full, slab);
                }

                return slab_obj_addr(slab, i);
            }
        }
    }

    /* 2. 尝试free（备用）slab */
    if (cache->free) {
        slab_t *slab = cache->free;
        list_remove(&cache->free, slab);
        list_prepend(&cache->partial, slab);

        /* 从中分配第一个对象 */
        uint8_t *bm = (uint8_t *)slab->page + SLAB_HDR_BASE;
        bitmap_clear(bm, 0);
        slab->obj_free--;
        cache->total_allocs++;
        return slab_obj_addr(slab, 0);
    }

    /* 3. 创建新的slab */
    slab_t *slab = slab_create(cache);
    if (!slab) {
        serial_puts("[slab] failed to create new slab\n");
        return NULL;
    }

    list_prepend(&cache->partial, slab);

    /* 分配第一个对象 */
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

    /* 在所有缓存中搜索拥有此指针的slab */
    for (int i = 0; i < SLAB_NUM_CACHES; i++) {
        slab_cache_t *cache = &caches[i];
        slab_t *slab = cache_find_slab(cache, ptr);
        if (!slab)
            continue;

        /* 从指针确定对象索引 */
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
            /* 已经空闲——可能是双重释放 */
            bitmap_set(bm, obj_idx);
            slab->obj_free++;
            cache->total_frees++;
            serial_puts("[slab] warning: possible double free\n");
            return;
        }

        /* 将对象标记为空闲 */
        bitmap_set(bm, obj_idx);
        slab->obj_free++;
        cache->total_frees++;

        /* 确定slab当前所在的链表，并按需移动 */
        int was_full = (slab->obj_free == 1);       /* 刚变为非满 */
        int became_free = (slab->obj_free == slab->obj_count); /* 完全空闲 */

        if (became_free) {
            /* 从当前所在的链表中移除 */
            list_remove(&cache->partial, slab);
            list_remove(&cache->full, slab);

            if (cache->free) {
                /* 已有备用空闲slab——释放此slab */
                free_page(slab->page);
            } else {
                /* 保留为备用 */
                list_prepend(&cache->free, slab);
            }
        } else if (was_full) {
            /* 原来在full链表，现在有空闲槽——移到partial */
            list_remove(&cache->full, slab);
            list_prepend(&cache->partial, slab);
        }
        /* 否则：已在partial链表中，保持不变 */

        return;
    }

    /* 如果执行到这里，说明指针不属于任何slab缓存。
     * 可能是通过alloc_pages()分配的大块内存。
     * 我们无法确定页数，因此只释放一页。
     * 对于大块分配，调用者应直接使用页面分配器API，
     * 或自行跟踪大小。 */
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

    /* 查找拥有此指针的缓存 */
    for (int i = 0; i < SLAB_NUM_CACHES; i++) {
        slab_cache_t *cache = &caches[i];
        slab_t *slab = cache_find_slab(cache, ptr);
        if (slab) {
            /* 当前分配大小为slab->obj_size */
            if (size <= slab->obj_size) {
                /* 适配当前slab对象 */
                return ptr;
            }
            /* 需要更大的分配 */
            void *new_ptr = slab_alloc(size);
            if (!new_ptr)
                return NULL;
            memcpy(new_ptr, ptr, slab->obj_size);
            slab_free(ptr);
            return new_ptr;
        }
    }

    /* 不在任何slab缓存中——是大块页面分配。
     * 我们不知道旧大小，因此无法复制。
     * 分配新的并返回；调用者必须自行处理。 */
    void *new_ptr = slab_alloc(size);
    if (!new_ptr)
        return NULL;
    /* 尽力而为：复制PAGE_SIZE字节（最小分配单位）。
     * 调用者确实应该自行管理大块分配。 */
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
