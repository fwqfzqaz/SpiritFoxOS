#include "drm.h"
#include "drm_gem.h"
#include "vga.h"
#include "fb.h"
#include "string.h"
#include "memory.h"
#include "timer.h"
#include "hal.h"

/* 刷新指定内存范围的 CPU 缓存行。
 * 用于解决 DRM GEM 缓冲区的缓存一致性问题：
 * 用户空间通过 uncached (PCD|PWT) 映射写入物理页面，
 * 但内核 vaddr 映射是 cached 的，可能读到过时的缓存数据。
 * 在 memcpy 前调用此函数确保读取最新数据。 */
static void flush_cache_range(void *addr, size_t len)
{
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)63;  /* 对齐到缓存行 */
    uintptr_t end   = ((uintptr_t)addr + len + 63) & ~(uintptr_t)63;
    for (uintptr_t p = start; p < end; p += 64) {
        __asm__ volatile("clflush (%0)" :: "r"(p) : "memory");
    }
    __asm__ volatile("sfence" ::: "memory");
}

/* ========================================================================
 * KMS - Kernel Mode Setting
 * 管理显示管线对象 (CRTC / Encoder / Connector) 和模式设置
 * ======================================================================== */

/* ---------- 辅助宏 ---------- */

/* 在环形事件队列中压入一个事件 */
static void kms_push_event(drm_event_vblank_t *evt)
{
    if (g_drm_dev.event_count >= DRM_EVENT_QUEUE_SIZE)
        return;  /* 队列满，丢弃 */

    g_drm_dev.events[g_drm_dev.event_tail] = *evt;
    g_drm_dev.event_tail = (g_drm_dev.event_tail + 1) % DRM_EVENT_QUEUE_SIZE;
    g_drm_dev.event_count++;
}

/* ---------- 初始化 ---------- */

void drm_kms_init(void)
{
    drm_crtc_t     *crtc     = &g_drm_dev.crtcs[0];
    drm_encoder_t  *encoder  = &g_drm_dev.encoders[0];
    drm_connector_t *connector = &g_drm_dev.connectors[0];

    /* CRTC: id=1 */
    memset(crtc, 0, sizeof(*crtc));
    crtc->id          = 1;
    crtc->enabled     = 0;
    crtc->fb_id       = 0;
    crtc->gamma_size  = 256;
    crtc->connector_id = 3;  /* 连接 Connector id=3 */

    /* Encoder: id=2, VIRTUAL 类型 */
    memset(encoder, 0, sizeof(*encoder));
    encoder->id             = 2;
    encoder->type           = DRM_MODE_ENCODER_VIRTUAL;
    encoder->crtc_id        = 1;  /* 连接 CRTC id=1 */
    encoder->possible_crtcs = 0x1;
    encoder->possible_clones = 0x1;

    /* Connector: id=3, VIRTUAL 类型, 已连接 */
    memset(connector, 0, sizeof(*connector));
    connector->id          = 3;
    connector->type        = DRM_MODE_CONNECTOR_VIRTUAL;
    connector->type_id     = 1;
    connector->connection  = DRM_MODE_CONNECTED;
    connector->encoder_id  = 2;
    connector->mm_width    = 260;   /* 模拟物理尺寸 ~10" */
    connector->mm_height   = 195;

    /* 添加默认模式: 1024x768@60Hz (32bpp) */
    drm_mode_modeinfo_t *mode = &connector->modes[0];
    memset(mode, 0, sizeof(*mode));
    mode->clock       = 65000;     /* kHz */
    mode->hdisplay    = 1024;
    mode->hsync_start = 1048;
    mode->hsync_end   = 1184;
    mode->htotal      = 1344;
    mode->vdisplay    = 768;
    mode->vsync_start = 771;
    mode->vsync_end   = 777;
    mode->vtotal      = 806;
    mode->vrefresh    = 60000;     /* x1000 Hz → 60 Hz */
    mode->type        = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
    mode->flags       = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
    strcpy(mode->name, "1024x768");
    connector->num_modes = 1;
}

/* ---------- GETRESOURCES ---------- */

int64_t drm_kms_get_resources(uint64_t arg)
{
    drm_mode_card_res_t *res = (drm_mode_card_res_t *)arg;

    res->count_fbs       = (uint32_t)g_drm_dev.gem_manager.num_framebuffers;
    res->count_crtcs     = 1;
    res->count_connectors = 1;
    res->count_encoders  = 1;

    res->width_mm   = 260;
    res->height_mm  = 195;
    res->min_width  = 640;
    res->max_width  = 1920;
    res->min_height = 480;
    res->max_height = 1080;

    /* 如果用户提供了足够大的数组，填充 ID */
    if (res->crtc_id_ptr && res->count_crtcs > 0) {
        uint32_t *ids = (uint32_t *)(uintptr_t)res->crtc_id_ptr;
        ids[0] = g_drm_dev.crtcs[0].id;
    }
    if (res->encoder_id_ptr && res->count_encoders > 0) {
        uint32_t *ids = (uint32_t *)(uintptr_t)res->encoder_id_ptr;
        ids[0] = g_drm_dev.encoders[0].id;
    }
    if (res->connector_id_ptr && res->count_connectors > 0) {
        uint32_t *ids = (uint32_t *)(uintptr_t)res->connector_id_ptr;
        ids[0] = g_drm_dev.connectors[0].id;
    }
    if (res->fb_id_ptr) {
        uint32_t *ids = (uint32_t *)(uintptr_t)res->fb_id_ptr;
        int idx = 0;
        for (int i = 0; i < 32 && idx < (int)res->count_fbs; i++) {
            if (g_drm_dev.gem_manager.framebuffers[i].id != 0)
                ids[idx++] = g_drm_dev.gem_manager.framebuffers[i].id;
        }
    }

    return 0;
}

/* ---------- GETCRTC ---------- */

int64_t drm_kms_get_crtc(uint64_t arg)
{
    drm_mode_crtc_t *c = (drm_mode_crtc_t *)arg;
    drm_crtc_t *crtc = &g_drm_dev.crtcs[0];

    c->crtc_id       = crtc->id;
    c->fb_id         = crtc->fb_id;
    c->x             = crtc->x;
    c->y             = crtc->y;
    c->gamma_size    = crtc->gamma_size;
    c->mode_valid    = crtc->enabled ? 1 : 0;
    if (crtc->enabled)
        c->mode = crtc->mode;

    return 0;
}

/* ---------- SETCRTC ---------- */

int64_t drm_kms_set_crtc(uint64_t arg)
{
    drm_mode_crtc_t *c = (drm_mode_crtc_t *)arg;
    drm_crtc_t *crtc = &g_drm_dev.crtcs[0];

    if (c->mode_valid && c->fb_id != 0) {
        /* 设置模式 */
        drm_framebuffer_t *fb = drm_fb_lookup(&g_drm_dev.gem_manager, c->fb_id);
        if (!fb) return -1;

        crtc->enabled = 1;
        crtc->fb_id   = c->fb_id;
        crtc->x       = c->x;
        crtc->y       = c->y;
        crtc->mode    = c->mode;
        crtc->connector_id = 3;

        /* 调用 Bochs 驱动设置硬件模式 */
        int ret = bochs_drm_set_mode(fb->width, fb->height, fb->bpp);
        if (ret != 0) return -1;

        /* 将帧缓冲内容复制到 LFB */
        if (fb->gem && fb->gem->vaddr) {
            uint64_t lfb = bochs_drm_get_lfb_addr();
            uint64_t copy_size = (uint64_t)fb->pitch * fb->height;
            flush_cache_range(fb->gem->vaddr, (size_t)copy_size);
            memcpy((void *)(uintptr_t)lfb, fb->gem->vaddr, (size_t)copy_size);
        }
        serial_puts("[SETCRTC] mode set OK\n");
        /* DRM 合成器已接管显示，禁止内核 printf 写入帧缓冲区
         * 以免 fb_term 写入破坏用户进程的代码页 */
        fb_term_disable_render();
    } else {
        /* 禁用 CRTC */
        crtc->enabled = 0;
        crtc->fb_id   = 0;
    }

    serial_puts("[SETCRTC] returning 0 to user\n");
    return 0;
}

/* ---------- GETENCODER ---------- */

int64_t drm_kms_get_encoder(uint64_t arg)
{
    drm_mode_get_encoder_t *e = (drm_mode_get_encoder_t *)arg;
    drm_encoder_t *enc = &g_drm_dev.encoders[0];

    e->encoder_id      = enc->id;
    e->encoder_type    = enc->type;
    e->crtc_id         = enc->crtc_id;
    e->possible_crtcs  = enc->possible_crtcs;
    e->possible_clones = enc->possible_clones;

    return 0;
}

/* ---------- GETCONNECTOR ---------- */

int64_t drm_kms_get_connector(uint64_t arg)
{
    drm_mode_get_connector_t *c = (drm_mode_get_connector_t *)arg;
    drm_connector_t *conn = &g_drm_dev.connectors[0];

    c->connector_id     = conn->id;
    c->connector_type   = conn->type;
    c->connector_type_id = conn->type_id;
    c->connection       = conn->connection;
    c->encoder_id       = conn->encoder_id;
    c->mm_width         = conn->mm_width;
    c->mm_height        = conn->mm_height;
    c->count_modes      = conn->num_modes;
    c->count_encoders   = 1;
    c->count_props      = 0;

    /* 填充模式数组 */
    if (c->modes_ptr && c->count_modes > 0) {
        drm_mode_modeinfo_t *dst = (drm_mode_modeinfo_t *)(uintptr_t)c->modes_ptr;
        for (uint32_t i = 0; i < conn->num_modes; i++)
            dst[i] = conn->modes[i];
    }

    /* 填充 encoder ID 数组 */
    if (c->encoders_ptr && c->count_encoders > 0) {
        uint32_t *ids = (uint32_t *)(uintptr_t)c->encoders_ptr;
        ids[0] = g_drm_dev.encoders[0].id;
    }

    return 0;
}

/* ---------- ADDFB ---------- */

int64_t drm_kms_addfb(uint64_t arg)
{
    drm_mode_fb_cmd_t *f = (drm_mode_fb_cmd_t *)arg;

    drm_framebuffer_t *fb = drm_fb_create(
        &g_drm_dev.gem_manager,
        f->width, f->height, f->pitch, f->bpp, f->depth,
        DRM_FORMAT_XRGB8888,
        f->handle);

    if (!fb) return -1;

    f->fb_id = fb->id;
    return 0;
}

/* ---------- ADDFB2 ---------- */

int64_t drm_kms_addfb2(uint64_t arg)
{
    drm_mode_fb_cmd2_t *f = (drm_mode_fb_cmd2_t *)arg;

    /* 简化：仅使用平面 0 */
    drm_framebuffer_t *fb = drm_fb_create(
        &g_drm_dev.gem_manager,
        f->width, f->height,
        f->pitches[0] ? f->pitches[0] : f->width * 4,
        32, 24,   /* bpp, depth */
        f->pixel_format,
        f->handles[0]);

    if (!fb) return -1;

    f->fb_id = fb->id;
    return 0;
}

/* ---------- RMFB ---------- */

int64_t drm_kms_rmfb(uint64_t arg)
{
    drm_mode_rmfb_t *r = (drm_mode_rmfb_t *)arg;
    drm_fb_destroy(&g_drm_dev.gem_manager, r->fb_id);
    return 0;
}

/* ---------- PAGE_FLIP ---------- */

int64_t drm_kms_page_flip(uint64_t arg)
{
    drm_mode_crtc_page_flip_t *pf = (drm_mode_crtc_page_flip_t *)arg;
    drm_crtc_t *crtc = &g_drm_dev.crtcs[0];

    /* 验证 FB */
    drm_framebuffer_t *fb = drm_fb_lookup(&g_drm_dev.gem_manager, pf->fb_id);
    if (!fb) return -1;

    /* 设置待翻转状态 */
    crtc->pending_fb_id     = pf->fb_id;
    crtc->pending_user_data = pf->user_data;
    crtc->page_flip_pending = 1;

    /* 若不请求事件，立即完成翻转 */
    if (!(pf->flags & DRM_MODE_PAGE_FLIP_EVENT)) {
        crtc->fb_id   = pf->fb_id;
        crtc->enabled = 1;

        /* 将新 FB 内容复制到 LFB */
        if (fb->gem && fb->gem->vaddr) {
            uint64_t lfb = bochs_drm_get_lfb_addr();
            uint64_t copy_size = (uint64_t)fb->pitch * fb->height;
            flush_cache_range(fb->gem->vaddr, (size_t)copy_size);
            memcpy((void *)(uintptr_t)lfb, fb->gem->vaddr, (size_t)copy_size);
        }

        crtc->page_flip_pending = 0;
    }

    return 0;
}

/* ---------- CREATE_DUMB ---------- */

int64_t drm_kms_create_dumb(uint64_t arg)
{
    drm_mode_create_dumb_t *d = (drm_mode_create_dumb_t *)arg;

    drm_gem_object_t *obj = drm_gem_create(
        &g_drm_dev.gem_manager, d->width, d->height, d->bpp);

    if (!obj) return -1;

    d->handle = obj->handle;
    d->pitch  = obj->pitch;
    d->size   = obj->size;

    return 0;
}

/* ---------- MAP_DUMB ---------- */

int64_t drm_kms_map_dumb(uint64_t arg)
{
    drm_mode_map_dumb_t *m = (drm_mode_map_dumb_t *)arg;

    uint64_t offset = drm_gem_get_mmap_offset(&g_drm_dev.gem_manager, m->handle);
    if (offset == 0) return -1;

    m->offset = offset;
    return 0;
}

/* ---------- DESTROY_DUMB ---------- */

int64_t drm_kms_destroy_dumb(uint64_t arg)
{
    drm_mode_destroy_dumb_t *d = (drm_mode_destroy_dumb_t *)arg;
    drm_gem_close(&g_drm_dev.gem_manager, d->handle);
    return 0;
}

/* ---------- VBlank 处理 ---------- */

void drm_kms_handle_vblank(void)
{
    drm_crtc_t *crtc = &g_drm_dev.crtcs[0];

    if (!crtc->page_flip_pending) return;

    /* 完成页面翻转 */
    drm_framebuffer_t *fb = drm_fb_lookup(
        &g_drm_dev.gem_manager, crtc->pending_fb_id);

    if (fb) {
        crtc->fb_id   = crtc->pending_fb_id;
        crtc->enabled = 1;

        /* 将新 FB 内容复制到 LFB */
        if (fb->gem && fb->gem->vaddr) {
            uint64_t lfb = bochs_drm_get_lfb_addr();
            uint64_t copy_size = (uint64_t)fb->pitch * fb->height;
            flush_cache_range(fb->gem->vaddr, (size_t)copy_size);
            memcpy((void *)(uintptr_t)lfb, fb->gem->vaddr, (size_t)copy_size);
        }
    }

    /* 生成翻转完成事件 */
    g_drm_dev.vblank_seq++;

    drm_event_vblank_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.base.type   = DRM_EVENT_FLIP_COMPLETE;
    evt.base.length = sizeof(evt);
    evt.user_data   = crtc->pending_user_data;
    evt.sequence    = g_drm_dev.vblank_seq;
    evt.crtc_id     = crtc->id;

    uint64_t ms = timer_get_ms();
    evt.tv_sec  = (uint32_t)(ms / 1000);
    evt.tv_usec = (uint32_t)((ms % 1000) * 1000);

    kms_push_event(&evt);

    crtc->page_flip_pending = 0;
}
