#ifndef DRM_KMS_H
#define DRM_KMS_H

#include <stdint.h>

/* ========================================================================
 * KMS - Kernel Mode Setting
 * Linux 兼容的 KMS 数据结构定义
 * ======================================================================== */

/* 显示模式信息 */
typedef struct {
    uint32_t clock;          /* 像素时钟 (kHz) */
    uint16_t hdisplay;       /* 水平显示宽度 */
    uint16_t hsync_start;    /* 水平同步开始 */
    uint16_t hsync_end;      /* 水平同步结束 */
    uint16_t htotal;         /* 水平总数 */
    uint16_t hskew;          /* 水平偏斜 */
    uint16_t vdisplay;       /* 垂直显示高度 */
    uint16_t vsync_start;    /* 垂直同步开始 */
    uint16_t vsync_end;      /* 垂直同步结束 */
    uint16_t vtotal;         /* 垂直总数 */
    uint16_t vscan;          /* 垂直扫描 */
    uint32_t vrefresh;       /* 刷新率 (Hz) */
    uint32_t flags;          /* 模式标志 */
    uint32_t type;           /* 模式类型 */
    char     name[32];       /* 模式名称 */
} drm_mode_modeinfo_t;

/* 模式类型 */
#define DRM_MODE_TYPE_BUILTIN      0x01
#define DRM_MODE_TYPE_CLOCK_C      0x02
#define DRM_MODE_TYPE_CRTC_C       0x04
#define DRM_MODE_TYPE_PREFERRED    0x08
#define DRM_MODE_TYPE_DEFAULT      0x10
#define DRM_MODE_TYPE_USERDEF      0x20
#define DRM_MODE_TYPE_DRIVER       0x40

/* 模式标志 */
#define DRM_MODE_FLAG_PHSYNC       0x0001
#define DRM_MODE_FLAG_NHSYNC       0x0002
#define DRM_MODE_FLAG_PVSYNC       0x0004
#define DRM_MODE_FLAG_NVSYNC       0x0008
#define DRM_MODE_FLAG_INTERLACE    0x0010
#define DRM_MODE_FLAG_DBLSCAN      0x0020

/* Connector 状态 */
#define DRM_MODE_CONNECTED         1
#define DRM_MODE_DISCONNECTED      2
#define DRM_MODE_UNKNOWNCONNECTION 3

/* Connector 类型 */
#define DRM_MODE_CONNECTOR_Unknown      0
#define DRM_MODE_CONNECTOR_VGA          1
#define DRM_MODE_CONNECTOR_DVII         2
#define DRM_MODE_CONNECTOR_DVID         3
#define DRM_MODE_CONNECTOR_DVIA         4
#define DRM_MODE_CONNECTOR_Composite    5
#define DRM_MODE_CONNECTOR_SVIDEO       6
#define DRM_MODE_CONNECTOR_LVDS         7
#define DRM_MODE_CONNECTOR_Component    8
#define DRM_MODE_CONNECTOR_9PinDIN      9
#define DRM_MODE_CONNECTOR_DisplayPort  10
#define DRM_MODE_CONNECTOR_HDMIA        11
#define DRM_MODE_CONNECTOR_HDMIB        12
#define DRM_MODE_CONNECTOR_TV           13
#define DRM_MODE_CONNECTOR_eDP          14
#define DRM_MODE_CONNECTOR_VIRTUAL      15

/* Encoder 类型 */
#define DRM_MODE_ENCODER_NONE    0
#define DRM_MODE_ENCODER_DAC     1
#define DRM_MODE_ENCODER_TMDS    2
#define DRM_MODE_ENCODER_LVDS    3
#define DRM_MODE_ENCODER_TVDAC   4
#define DRM_MODE_ENCODER_VIRTUAL 5

/* GETRESOURCES ioctl 结构 (Linux x86_64: 指针字段为 __u64) */
typedef struct {
    uint64_t fb_id_ptr;       /* 输出：FB ID 数组指针 */
    uint64_t crtc_id_ptr;     /* 输出：CRTC ID 数组指针 */
    uint64_t connector_id_ptr;/* 输出：Connector ID 数组指针 */
    uint64_t encoder_id_ptr;  /* 输出：Encoder ID 数组指针 */
    uint32_t count_fbs;       /* 输入/输出：FB 数量 */
    uint32_t count_crtcs;     /* 输入/输出：CRTC 数量 */
    uint32_t count_connectors;/* 输入/输出：Connector 数量 */
    uint32_t count_encoders;  /* 输入/输出：Encoder 数量 */
    uint32_t width_mm;        /* 输出：显示宽度 (mm) */
    uint32_t height_mm;       /* 输出：显示高度 (mm) */
    uint32_t min_width;       /* 输出：最小宽度 */
    uint32_t max_width;       /* 输出：最大宽度 */
    uint32_t min_height;      /* 输出：最小高度 */
    uint32_t max_height;      /* 输出：最大高度 */
} drm_mode_card_res_t;

/* CRTC ioctl 结构 */
typedef struct {
    uint64_t set_connectors_ptr;  /* 输入：连接的 Connector ID 数组 */
    uint32_t count_connectors;    /* 输入：Connector 数量 */
    uint32_t crtc_id;             /* 输入/输出：CRTC ID */
    uint32_t fb_id;               /* 输入/输出：FB ID */
    uint32_t x;                   /* 输入/输出：X 偏移 */
    uint32_t y;                   /* 输入/输出：Y 偏移 */
    uint32_t gamma_size;          /* 输出：Gamma 表大小 */
    uint32_t mode_valid;          /* 输入/输出：模式是否有效 */
    drm_mode_modeinfo_t mode;     /* 输入/输出：显示模式 */
} drm_mode_crtc_t;

/* Encoder ioctl 结构 */
typedef struct {
    uint32_t encoder_id;      /* 输入/输出：Encoder ID */
    uint32_t encoder_type;    /* 输出：Encoder 类型 */
    uint32_t crtc_id;         /* 输出：关联的 CRTC ID */
    uint32_t possible_crtcs;  /* 输出：可用的 CRTC 位掩码 */
    uint32_t possible_clones; /* 输出：可克隆的 Encoder 位掩码 */
} drm_mode_get_encoder_t;

/* Connector ioctl 结构 */
typedef struct {
    uint64_t encoders_ptr;        /* 输出：Encoder ID 数组 */
    uint64_t modes_ptr;           /* 输出：模式数组 */
    uint64_t props_ptr;           /* 输出：属性 ID 数组 */
    uint64_t prop_values_ptr;     /* 输出：属性值数组 */
    uint32_t count_modes;         /* 输入/输出：模式数量 */
    uint32_t count_props;         /* 输入/输出：属性数量 */
    uint32_t count_encoders;      /* 输入/输出：Encoder 数量 */
    uint32_t encoder_id;          /* 输入/输出：当前 Encoder ID */
    uint32_t connector_id;        /* 输入/输出：Connector ID */
    uint32_t connector_type;      /* 输出：Connector 类型 */
    uint32_t connector_type_id;   /* 输出：Connector 类型 ID */
    uint32_t connection;          /* 输出：连接状态 */
    uint32_t mm_width;            /* 输出：物理宽度 (mm) */
    uint32_t mm_height;           /* 输出：物理高度 (mm) */
    uint32_t subpixel;            /* 输出：子像素排列 */
    uint32_t pad;
} drm_mode_get_connector_t;

/* Framebuffer 添加 (旧版) */
typedef struct {
    uint32_t fb_id;       /* 输出：创建的 FB ID */
    uint32_t width;       /* 输入：宽度 */
    uint32_t height;      /* 输入：高度 */
    uint32_t pitch;       /* 输入：行字节数 */
    uint32_t bpp;         /* 输入：位深 */
    uint32_t depth;       /* 输入：颜色深度 */
    uint32_t handle;      /* 输入：GEM 句柄 */
} drm_mode_fb_cmd_t;

/* Framebuffer 添加 (新版，支持多平面和像素格式) */
typedef struct {
    uint32_t fb_id;           /* 输出：创建的 FB ID */
    uint32_t width;           /* 输入：宽度 */
    uint32_t height;          /* 输入：高度 */
    uint32_t pixel_format;    /* 输入：像素格式 (四字符码) */
    uint32_t flags;           /* 输入：标志 */
    uint32_t handles[4];      /* 输入：GEM 句柄 */
    uint32_t pitches[4];      /* 输入：每行字节数 */
    uint32_t offsets[4];      /* 输入：平面偏移 */
    uint64_t modifier[4];     /* 输入：格式修饰符 */
} drm_mode_fb_cmd2_t;

/* 移除 Framebuffer */
typedef struct {
    uint32_t fb_id;       /* 输入：要移除的 FB ID */
    uint32_t pad;
} drm_mode_rmfb_t;

/* 标记 Framebuffer 脏区域 */
typedef struct {
    uint32_t fb_id;            /* 输入：FB ID */
    uint32_t flags;            /* 输入：标志 */
    uint32_t color;            /* 输入：颜色 (用于清除) */
    uint32_t num_clips;        /* 输入：脏区域数量 */
    uint64_t clips_ptr;        /* 输入：脏区域数组指针 */
} drm_mode_fb_dirty_cmd_t;

/* 页面翻转 */
#define DRM_MODE_PAGE_FLIP_EVENT  0x01
#define DRM_MODE_PAGE_FLIP_ASYNC  0x02
#define DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE 0x4
#define DRM_MODE_PAGE_FLIP_TARGET_RELATIVE 0x8

typedef struct {
    uint32_t fb_id;            /* 输入：目标 FB ID */
    uint32_t crtc_id;          /* 输入：CRTC ID */
    uint32_t flags;            /* 输入：标志 */
    uint32_t sequence;         /* 输入：序列号 */
    uint64_t user_data;        /* 输入：用户数据 (随事件返回) */
} drm_mode_crtc_page_flip_t;

/* Dumb Buffer 创建 */
typedef struct {
    uint32_t height;       /* 输入：高度 */
    uint32_t width;        /* 输入：宽度 */
    uint32_t bpp;          /* 输入：位深 */
    uint32_t flags;        /* 输入：标志 */
    uint32_t handle;       /* 输出：GEM 句柄 */
    uint32_t pitch;        /* 输出：行字节数 */
    uint64_t size;         /* 输出：缓冲区总大小 */
} drm_mode_create_dumb_t;

/* Dumb Buffer 映射 */
typedef struct {
    uint32_t handle;       /* 输入：GEM 句柄 */
    uint32_t pad;
    uint64_t offset;       /* 输出：mmap 偏移量 */
} drm_mode_map_dumb_t;

/* Dumb Buffer 销毁 */
typedef struct {
    uint32_t handle;       /* 输入：GEM 句柄 */
    uint32_t pad;
} drm_mode_destroy_dumb_t;

/* 原子模式设置 (指针字段为 __u64) */
typedef struct {
    uint32_t flags;            /* 输入：标志 */
    uint32_t count_objs;       /* 输入：对象数量 */
    uint64_t objs_ptr;         /* 输入：对象 ID 数组指针 */
    uint32_t count_props;      /* 输入：属性数量 */
    uint32_t pad;
    uint64_t props_ptr;        /* 输入：属性 ID 数组指针 */
    uint64_t prop_values_ptr;  /* 输入：属性值数组指针 */
    uint64_t user_data;        /* 输入：用户数据 */
} drm_mode_atomic_t;

/* 像素格式四字符码 */
#define DRM_FORMAT_fourcc(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define DRM_FORMAT_XRGB8888  DRM_FORMAT_fourcc('X', 'R', '2', '4')
#define DRM_FORMAT_ARGB8888  DRM_FORMAT_fourcc('A', 'R', '2', '4')
#define DRM_FORMAT_RGB565    DRM_FORMAT_fourcc('R', 'G', '1', '6')
#define DRM_FORMAT_XBGR8888  DRM_FORMAT_fourcc('X', 'B', '2', '4')

#endif /* DRM_KMS_H */
