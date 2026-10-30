#include "drm_gem.h"
#include "hal.h"
#include "memory.h"
#include "string.h"
#include "vga.h"

/* ========================================================================
 * GEM - Graphics Execution Manager
 * 图形缓冲区对象管理
 * ======================================================================== */

/* ---------- GEM 管理器 ---------- */

void drm_gem_manager_init(drm_gem_manager_t *mgr)
{
    memset(mgr, 0, sizeof(*mgr));
    mgr->next_fb_id      = 1;
    mgr->next_gem_name   = 1;
    mgr->next_mmap_offset = 0x10000000ULL;
}

drm_gem_object_t *drm_gem_create(drm_gem_manager_t *mgr,
                                  uint32_t width, uint32_t height, uint32_t bpp)
{
    /* 在对象池中查找空闲槽位 */
    int index = -1;
    for (int i = 0; i < 64; i++) {
        if (mgr->objects[i].refcount == 0) {
            index = i;
            break;
        }
    }
    if (index < 0) return NULL;

    /* 计算 pitch：width * (bpp/8)，4 字节对齐 */
    uint32_t pitch = width * (bpp / 8);
    pitch = (pitch + 3) & ~3U;

    /* 计算总大小 */
    uint64_t size = (uint64_t)pitch * height;

    /* 分配物理页 */
    size_t num_pages = ((size_t)size + PAGE_SIZE - 1) / PAGE_SIZE;
    void *vaddr = alloc_pages(num_pages);
    if (!vaddr) return NULL;

    /* 确保内存已映射 */
    hal_ensure_mapped((uintptr_t)vaddr, (size_t)size);

    /* 清零缓冲区 */
    memset(vaddr, 0, num_pages * PAGE_SIZE);

    /* 初始化 GEM 对象 */
    drm_gem_object_t *obj = &mgr->objects[index];
    memset(obj, 0, sizeof(*obj));

    obj->handle     = (uint32_t)(index + 1);  /* 1-based 句柄 */
    obj->name       = 0;
    obj->size       = size;
    obj->pitch      = pitch;
    obj->width      = width;
    obj->height     = height;
    obj->bpp        = bpp;
    obj->vaddr      = vaddr;
    obj->paddr      = (uint64_t)(uintptr_t)vaddr;  /* 恒等映射 */
    obj->pages      = (uint32_t)num_pages;
    obj->refcount   = 1;
    obj->mmap_offset = 0;
    obj->fb_id      = 0;

    mgr->num_objects++;

    return obj;
}

drm_gem_object_t *drm_gem_lookup(drm_gem_manager_t *mgr, uint32_t handle)
{
    if (handle == 0 || handle > 64) return NULL;
    drm_gem_object_t *obj = &mgr->objects[handle - 1];
    if (obj->refcount == 0) return NULL;
    return obj;
}

void drm_gem_close(drm_gem_manager_t *mgr, uint32_t handle)
{
    drm_gem_object_t *obj = drm_gem_lookup(mgr, handle);
    if (!obj) return;

    obj->refcount--;
    if (obj->refcount <= 0) {
        /* 释放物理页 */
        if (obj->vaddr) {
            for (uint32_t i = 0; i < obj->pages; i++) {
                void *page = (void *)((uintptr_t)obj->vaddr + (uintptr_t)i * PAGE_SIZE);
                free_page(page);
            }
        }

        /* 若关联了 FB，清除引用 */
        if (obj->fb_id != 0) {
            drm_fb_destroy(mgr, obj->fb_id);
        }

        memset(obj, 0, sizeof(*obj));
        mgr->num_objects--;
    }
}

uint64_t drm_gem_get_mmap_offset(drm_gem_manager_t *mgr, uint32_t handle)
{
    drm_gem_object_t *obj = drm_gem_lookup(mgr, handle);
    if (!obj) return 0;

    /* 若已分配 mmap 偏移则直接返回 */
    if (obj->mmap_offset != 0) return obj->mmap_offset;

    /* 分配新的唯一偏移 */
    obj->mmap_offset = mgr->next_mmap_offset;
    mgr->next_mmap_offset += obj->size;
    /* 4KB 对齐 */
    mgr->next_mmap_offset = (mgr->next_mmap_offset + 0xFFFULL) & ~0xFFFULL;

    return obj->mmap_offset;
}

/* ---------- Framebuffer 操作 ---------- */

drm_framebuffer_t *drm_fb_create(drm_gem_manager_t *mgr,
                                  uint32_t width, uint32_t height,
                                  uint32_t pitch, uint32_t bpp, uint32_t depth,
                                  uint32_t pixel_format, uint32_t gem_handle)
{
    /* 查找 GEM 对象 */
    drm_gem_object_t *gem = drm_gem_lookup(mgr, gem_handle);
    if (!gem) return NULL;

    /* 在 FB 池中查找空闲槽位 */
    int index = -1;
    for (int i = 0; i < 32; i++) {
        if (mgr->framebuffers[i].id == 0) {
            index = i;
            break;
        }
    }
    if (index < 0) return NULL;

    /* 分配 FB ID */
    uint32_t fb_id = mgr->next_fb_id++;

    /* 初始化 framebuffer */
    drm_framebuffer_t *fb = &mgr->framebuffers[index];
    memset(fb, 0, sizeof(*fb));

    fb->id           = fb_id;
    fb->width        = width;
    fb->height       = height;
    fb->pitch        = pitch;
    fb->pixel_format = pixel_format;
    fb->depth        = depth;
    fb->bpp          = bpp;
    fb->gem          = gem;

    /* 关联 FB 到 GEM 对象 */
    gem->fb_id = fb_id;

    mgr->num_framebuffers++;

    return fb;
}

drm_framebuffer_t *drm_fb_lookup(drm_gem_manager_t *mgr, uint32_t fb_id)
{
    if (fb_id == 0) return NULL;
    for (int i = 0; i < 32; i++) {
        if (mgr->framebuffers[i].id == fb_id)
            return &mgr->framebuffers[i];
    }
    return NULL;
}

void drm_fb_destroy(drm_gem_manager_t *mgr, uint32_t fb_id)
{
    drm_framebuffer_t *fb = drm_fb_lookup(mgr, fb_id);
    if (!fb) return;

    /* 清除 GEM 对象中的 FB 引用 */
    if (fb->gem && fb->gem->fb_id == fb_id)
        fb->gem->fb_id = 0;

    memset(fb, 0, sizeof(*fb));
    mgr->num_framebuffers--;
}
