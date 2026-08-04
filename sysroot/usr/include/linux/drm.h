#ifndef _LINUX_DRM_H
#define _LINUX_DRM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>

/* ========================================================================
 * DRM - Direct Rendering Manager
 * Linux 兼容的 DRM/KMS 用户态接口定义
 *
 * 此头文件供 libdrm 及用户态图形程序使用，
 * 与内核 kernel/include/drm.h 和 drm_kms.h 保持兼容。
 * ======================================================================== */

#define DRM_IOCTL_BASE          'd'
#define DRM_IO(nr)              _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOW(nr, type)       _IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOR(nr, type)       _IOR(DRM_IOCTL_BASE, nr, type)
#define DRM_IOWR(nr, type)      _IOWR(DRM_IOCTL_BASE, nr, type)

/* DRM ioctl 命令编号 */
#define DRM_IOCTL_VERSION         0x00
#define DRM_IOCTL_GET_UNIQUE      0x01
#define DRM_IOCTL_GET_MAGIC       0x02
#define DRM_IOCTL_IRQ_BUSID       0x03
#define DRM_IOCTL_GET_MAP         0x04
#define DRM_IOCTL_GET_CLIENT      0x05
#define DRM_IOCTL_GET_STATS       0x06
#define DRM_IOCTL_SET_VERSION     0x07
#define DRM_IOCTL_MODESET_CTL     0x08
#define DRM_IOCTL_GEM_CLOSE       0x09
#define DRM_IOCTL_GEM_FLINK       0x0a
#define DRM_IOCTL_GEM_OPEN        0x0b
#define DRM_IOCTL_GET_CAP         0x0c
#define DRM_IOCTL_SET_CLIENT_CAP  0x0d

/* DRM Mode Setting ioctls */
#define DRM_IOCTL_MODE_GETRESOURCES          0xa0
#define DRM_IOCTL_MODE_GETCRTC               0xa1
#define DRM_IOCTL_MODE_SETCRTC               0xa2
#define DRM_IOCTL_MODE_CURSOR                0xa3
#define DRM_IOCTL_MODE_CURSOR2               0xa4
#define DRM_IOCTL_MODE_GETGAMMA              0xa5
#define DRM_IOCTL_MODE_SETGAMMA              0xa6
#define DRM_IOCTL_MODE_GETENCODER            0xa7
#define DRM_IOCTL_MODE_GETCONNECTOR          0xa8
#define DRM_IOCTL_MODE_ATTACHMODE            0xa9
#define DRM_IOCTL_MODE_DETACHMODE            0xaa
#define DRM_IOCTL_MODE_GETPROPERTY           0xab
#define DRM_IOCTL_MODE_SETPROPERTY           0xac
#define DRM_IOCTL_MODE_GETPROPBLOB           0xad
#define DRM_IOCTL_MODE_GETFB                 0xae
#define DRM_IOCTL_MODE_ADDFB                 0xaf
#define DRM_IOCTL_MODE_RMFB                  0xb0
#define DRM_IOCTL_MODE_PAGE_FLIP             0xb1
#define DRM_IOCTL_MODE_DIRTYFB               0xb2
#define DRM_IOCTL_MODE_CREATE_DUMB           0xb3
#define DRM_IOCTL_MODE_MAP_DUMB              0xb4
#define DRM_IOCTL_MODE_DESTROY_DUMB          0xb5
#define DRM_IOCTL_MODE_GETPLANERESOURCES     0xb6
#define DRM_IOCTL_MODE_GETPLANE              0xb7
#define DRM_IOCTL_MODE_SETPLANE              0xb8
#define DRM_IOCTL_MODE_ADDFB2                0xb9
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES     0xba
#define DRM_IOCTL_MODE_OBJ_SETPROPERTY       0xbb
#define DRM_IOCTL_MODE_CURSOR2_CMD           0xbc
#define DRM_IOCTL_MODE_ATOMIC                0xc0

/* 完整的 ioctl 命令码 */
#define DRM_IOCTL_VERSION_NR         DRM_IOWR(DRM_IOCTL_VERSION, struct drm_version)
#define DRM_IOCTL_GET_CAP_NR         DRM_IOWR(DRM_IOCTL_GET_CAP, struct drm_get_cap)
#define DRM_IOCTL_SET_CLIENT_CAP_NR  DRM_IOW(DRM_IOCTL_SET_CLIENT_CAP, struct drm_set_client_cap)
#define DRM_IOCTL_GEM_CLOSE_NR       DRM_IOW(DRM_IOCTL_GEM_CLOSE, struct drm_gem_close)
#define DRM_IOCTL_GEM_FLINK_NR       DRM_IOWR(DRM_IOCTL_GEM_FLINK, struct drm_gem_flink)
#define DRM_IOCTL_GEM_OPEN_NR        DRM_IOWR(DRM_IOCTL_GEM_OPEN, struct drm_gem_open)
#define DRM_IOCTL_MODESET_CTL_NR     DRM_IOW(DRM_IOCTL_MODESET_CTL, struct drm_modeset_ctl)

#define DRM_IOCTL_MODE_GETRESOURCES_NR    DRM_IOWR(DRM_IOCTL_MODE_GETRESOURCES, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC_NR         DRM_IOWR(DRM_IOCTL_MODE_GETCRTC, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC_NR         DRM_IOWR(DRM_IOCTL_MODE_SETCRTC, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER_NR      DRM_IOWR(DRM_IOCTL_MODE_GETENCODER, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR_NR    DRM_IOWR(DRM_IOCTL_MODE_GETCONNECTOR, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB_NR           DRM_IOWR(DRM_IOCTL_MODE_ADDFB, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_ADDFB2_NR          DRM_IOWR(DRM_IOCTL_MODE_ADDFB2, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_MODE_RMFB_NR            DRM_IOWR(DRM_IOCTL_MODE_RMFB, uint32_t)
#define DRM_IOCTL_MODE_DIRTYFB_NR         DRM_IOWR(DRM_IOCTL_MODE_DIRTYFB, struct drm_mode_fb_dirty_cmd)
#define DRM_IOCTL_MODE_PAGE_FLIP_NR       DRM_IOWR(DRM_IOCTL_MODE_PAGE_FLIP, struct drm_mode_crtc_page_flip)
#define DRM_IOCTL_MODE_CREATE_DUMB_NR     DRM_IOWR(DRM_IOCTL_MODE_CREATE_DUMB, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB_NR        DRM_IOWR(DRM_IOCTL_MODE_MAP_DUMB, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB_NR    DRM_IOWR(DRM_IOCTL_MODE_DESTROY_DUMB, struct drm_mode_destroy_dumb)
#define DRM_IOCTL_MODE_GETFB_NR           DRM_IOWR(DRM_IOCTL_MODE_GETFB, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_ATOMIC_NR          DRM_IOWR(DRM_IOCTL_MODE_ATOMIC, struct drm_mode_atomic)

/* DRM 版本信息 */
struct drm_version {
    int      version_major;
    int      version_minor;
    int      version_patchlevel;
    size_t   name_len;
    char    *name;
    size_t   date_len;
    char    *date;
    size_t   desc_len;
    char    *desc;
};

/* DRM 能力查询 */
#define DRM_CAP_DUMB_BUFFER           0x1
#define DRM_CAP_VBLANK_HIGH_CRTC      0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH  0x3
#define DRM_CAP_DUMB_PREFER_SHADOW    0x4
#define DRM_CAP_PRIME                 0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC   0x6
#define DRM_CAP_ASYNC_PAGE_FLIP       0x7

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

struct drm_set_client_cap {
    uint64_t capability;
    uint64_t value;
};

/* Modeset 控制 */
struct drm_modeset_ctl {
    uint32_t crtc;
    uint32_t cmd;
};

/* GEM 操作 */
struct drm_gem_close {
    uint32_t handle;
    uint32_t pad;
};

struct drm_gem_flink {
    uint32_t handle;
    uint32_t name;
};

struct drm_gem_open {
    uint32_t name;
    uint32_t handle;
    uint64_t size;
};

/* DRM 事件类型 */
#define DRM_EVENT_VBLANK        0x01
#define DRM_EVENT_FLIP_COMPLETE 0x02

struct drm_event {
    uint32_t type;
    uint32_t length;
};

struct drm_event_vblank {
    struct drm_event base;
    uint64_t user_data;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t sequence;
    uint32_t crtc_id;
};

/* DRM 设备权限标志 */
#define DRM_MASTER      0x01
#define DRM_ROOT_ONLY   0x02

/* ========================================================================
 * KMS 数据结构
 * ======================================================================== */

/* 显示模式信息 */
struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char     name[32];
};

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
struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t width_mm;
    uint32_t height_mm;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
};

/* CRTC ioctl 结构 */
struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_modeinfo mode;
};

/* Encoder ioctl 结构 */
struct drm_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
};

/* Connector ioctl 结构 */
struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

/* Framebuffer 添加 (旧版) */
struct drm_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
};

/* Framebuffer 添加 (新版，支持多平面和像素格式) */
struct drm_mode_fb_cmd2 {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
};

/* 标记 Framebuffer 脏区域 */
struct drm_mode_fb_dirty_cmd {
    uint32_t fb_id;
    uint32_t flags;
    uint32_t color;
    uint32_t num_clips;
    uint64_t clips_ptr;
};

/* 页面翻转 */
#define DRM_MODE_PAGE_FLIP_EVENT          0x01
#define DRM_MODE_PAGE_FLIP_ASYNC          0x02
#define DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE 0x4
#define DRM_MODE_PAGE_FLIP_TARGET_RELATIVE 0x8

struct drm_mode_crtc_page_flip {
    uint32_t fb_id;
    uint32_t crtc_id;
    uint32_t flags;
    uint32_t sequence;
    uint64_t user_data;
};

/* Dumb Buffer 创建 */
struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

/* Dumb Buffer 映射 */
struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

/* Dumb Buffer 销毁 */
struct drm_mode_destroy_dumb {
    uint32_t handle;
    uint32_t pad;
};

/* 原子模式设置 */
struct drm_mode_atomic {
    uint32_t flags;
    uint32_t count_objs;
    uint64_t objs_ptr;
    uint32_t count_props;
    uint32_t pad;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint64_t user_data;
};

/* 像素格式四字符码 */
#define DRM_FORMAT_fourcc(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define DRM_FORMAT_XRGB8888  DRM_FORMAT_fourcc('X', 'R', '2', '4')
#define DRM_FORMAT_ARGB8888  DRM_FORMAT_fourcc('A', 'R', '2', '4')
#define DRM_FORMAT_RGB565    DRM_FORMAT_fourcc('R', 'G', '1', '6')
#define DRM_FORMAT_XBGR8888  DRM_FORMAT_fourcc('X', 'B', '2', '4')

/* DRM 设备节点路径 */
#define DRM_DEV_CARD0    "/dev/dri/card0"
#define DRM_DEV_CONTROL  "/dev/dri/controlD64"
#define DRM_DEV_RENDER   "/dev/dri/renderD128"

#endif /* _LINUX_DRM_H */
