#ifndef DRM_GEM_H
#define DRM_GEM_H

#include <stdint.h>
#include "drm_kms.h"

/* ========================================================================
 * GEM - Graphics Execution Manager
 * 图形缓冲区对象管理
 * ======================================================================== */

/* GEM 缓冲区对象 (Buffer Object) */
typedef struct drm_gem_object {
    uint32_t handle;        /* 进程内句柄 */
    uint32_t name;          /* 全局名称 (flink) */
    uint64_t size;          /* 缓冲区大小 (字节) */
    uint32_t pitch;         /* 行字节数 (dumb buffer) */
    uint32_t width;         /* 宽度 (dumb buffer) */
    uint32_t height;        /* 高度 (dumb buffer) */
    uint32_t bpp;           /* 位深 (dumb buffer) */

    /* 物理内存信息 */
    void     *vaddr;        /* 内核虚拟地址 */
    uint64_t paddr;         /* 物理地址 (0 = 非连续) */
    uint32_t pages;         /* 占用的页数 */

    /* 引用计数 */
    int refcount;

    /* mmap 偏移 */
    uint64_t mmap_offset;

    /* 关联的 FB ID (0 = 未关联) */
    uint32_t fb_id;
} drm_gem_object_t;

/* DRM Framebuffer 对象 */
typedef struct drm_framebuffer {
    uint32_t id;                /* FB ID */
    uint32_t width;             /* 宽度 */
    uint32_t height;            /* 高度 */
    uint32_t pitch;             /* 行字节数 */
    uint32_t pixel_format;      /* 像素格式 */
    uint32_t depth;             /* 颜色深度 */
    uint32_t bpp;               /* 位深 */
    drm_gem_object_t *gem;      /* 关联的 GEM 对象 */
} drm_framebuffer_t;

/* CRTC 对象 */
typedef struct drm_crtc {
    uint32_t id;                /* CRTC ID */
    int enabled;                /* 是否启用 */
    uint32_t fb_id;             /* 当前显示的 FB ID */
    uint32_t x;                 /* X 偏移 */
    uint32_t y;                 /* Y 偏移 */
    drm_mode_modeinfo_t mode;   /* 当前显示模式 */
    uint32_t gamma_size;        /* Gamma 表大小 */

    /* 双缓冲页面翻转 */
    uint32_t pending_fb_id;     /* 待翻转的 FB ID */
    uint64_t pending_user_data; /* 翻转完成事件的用户数据 */
    int page_flip_pending;      /* 是否有待完成的翻转 */

    /* 关联的 Connector */
    uint32_t connector_id;
} drm_crtc_t;

/* Encoder 对象 */
typedef struct drm_encoder {
    uint32_t id;                /* Encoder ID */
    uint32_t type;              /* Encoder 类型 */
    uint32_t crtc_id;           /* 关联的 CRTC ID */
    uint32_t possible_crtcs;    /* 可用的 CRTC 位掩码 */
    uint32_t possible_clones;   /* 可克隆位掩码 */
} drm_encoder_t;

/* Connector 对象 */
typedef struct drm_connector {
    uint32_t id;                /* Connector ID */
    uint32_t type;              /* Connector 类型 */
    uint32_t type_id;           /* 类型内 ID */
    uint32_t connection;        /* 连接状态 */
    uint32_t encoder_id;        /* 当前 Encoder ID */
    uint32_t mm_width;          /* 物理宽度 (mm) */
    uint32_t mm_height;         /* 物理高度 (mm) */

    /* 支持的模式 */
    drm_mode_modeinfo_t modes[16]; /* 支持的模式列表 */
    uint32_t num_modes;         /* 模式数量 */
} drm_connector_t;

/* GEM 管理器状态 */
typedef struct drm_gem_manager {
    drm_gem_object_t  objects[64];  /* GEM 对象池 */
    drm_framebuffer_t framebuffers[32]; /* Framebuffer 池 */
    int num_objects;
    int num_framebuffers;

    /* mmap 偏移计数器 */
    uint64_t next_mmap_offset;

    /* 下一个 ID */
    uint32_t next_fb_id;
    uint32_t next_gem_name;
} drm_gem_manager_t;

/* GEM 管理器操作 */
void drm_gem_manager_init(drm_gem_manager_t *mgr);
drm_gem_object_t *drm_gem_create(drm_gem_manager_t *mgr, uint32_t width,
                                 uint32_t height, uint32_t bpp);
drm_gem_object_t *drm_gem_lookup(drm_gem_manager_t *mgr, uint32_t handle);
void drm_gem_close(drm_gem_manager_t *mgr, uint32_t handle);
uint64_t drm_gem_get_mmap_offset(drm_gem_manager_t *mgr, uint32_t handle);

/* Framebuffer 操作 */
drm_framebuffer_t *drm_fb_create(drm_gem_manager_t *mgr, uint32_t width,
                                 uint32_t height, uint32_t pitch,
                                 uint32_t bpp, uint32_t depth,
                                 uint32_t pixel_format, uint32_t gem_handle);
drm_framebuffer_t *drm_fb_lookup(drm_gem_manager_t *mgr, uint32_t fb_id);
void drm_fb_destroy(drm_gem_manager_t *mgr, uint32_t fb_id);

/* ========================================================================
 * DRM 设备 - Device
 * ======================================================================== */

#include "drm.h"

/* DRM 事件队列大小 */
#define DRM_EVENT_QUEUE_SIZE 16

/* DRM 设备结构 */
typedef struct drm_device {
    drm_gem_manager_t gem_manager;
    drm_crtc_t        crtcs[1];
    drm_encoder_t     encoders[1];
    drm_connector_t   connectors[1];

    /* 事件队列 */
    drm_event_vblank_t events[DRM_EVENT_QUEUE_SIZE];
    int event_head;
    int event_tail;
    int event_count;

    /* VBlank 序列号 */
    uint32_t vblank_seq;
} drm_device_t;

extern drm_device_t g_drm_dev;

/* ========================================================================
 * Bochs DRM 驱动接口
 * ======================================================================== */

int     bochs_drm_init(void);
int     bochs_drm_set_mode(uint32_t width, uint32_t height, uint32_t bpp);
uint64_t bochs_drm_get_lfb_addr(void);
void    bochs_drm_page_flip(uint64_t offset);

/* ========================================================================
 * KMS 操作接口
 * ======================================================================== */

void     drm_kms_init(void);
int64_t  drm_kms_get_resources(uint64_t arg);
int64_t  drm_kms_get_crtc(uint64_t arg);
int64_t  drm_kms_set_crtc(uint64_t arg);
int64_t  drm_kms_get_encoder(uint64_t arg);
int64_t  drm_kms_get_connector(uint64_t arg);
int64_t  drm_kms_addfb(uint64_t arg);
int64_t  drm_kms_addfb2(uint64_t arg);
int64_t  drm_kms_rmfb(uint64_t arg);
int64_t  drm_kms_page_flip(uint64_t arg);
int64_t  drm_kms_create_dumb(uint64_t arg);
int64_t  drm_kms_map_dumb(uint64_t arg);
int64_t  drm_kms_destroy_dumb(uint64_t arg);
void     drm_kms_handle_vblank(void);

/* DRM 核心初始化 */
void     drm_init(void);

#endif /* DRM_GEM_H */
