#ifndef DRM_H
#define DRM_H

#include <stdint.h>
#include <stddef.h>

/* ========================================================================
 * DRM - Direct Rendering Manager
 * Linux 兼容的 DRM ioctl 定义
 * ======================================================================== */

#define DRM_IOCTL_BASE          'd'
#define DRM_IO(nr)              _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOW(nr, type)       _IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOR(nr, type)       _IOR(DRM_IOCTL_BASE, nr, type)
#define DRM_IOWR(nr, type)      _IOWR(DRM_IOCTL_BASE, nr, type)

/* _IO/_IOW/_IOR/_IOWR 宏 (Linux asm-generic/ioctl.h 兼容)
 * 标准 Linux ioctl 编码 (x86_64):
 *   bits 7:0   = nr   (命令编号)
 *   bits 15:8  = type (类型魔数)
 *   bits 29:16 = size (参数大小, 14 bits)
 *   bits 31:30 = dir  (方向, 2 bits)
 */
#define _IOC_NRBITS     8
#define _IOC_TYPEBITS   8
#define _IOC_SIZEBITS   14
#define _IOC_DIRBITS    2

#define _IOC_NRSHIFT    0
#define _IOC_TYPESHIFT  _IOC_NRBITS
#define _IOC_SIZESHIFT  (_IOC_NRBITS + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT   (_IOC_NRBITS + _IOC_TYPEBITS + _IOC_SIZEBITS)

#define _IOC_NRMASK     ((1 << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK   ((1 << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK   ((1 << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK    ((1 << _IOC_DIRBITS) - 1)

#define _IOC_NONE       0
#define _IOC_WRITE      1
#define _IOC_READ       2

#define _IOC(dir, type, nr, size) \
    (((dir)  << _IOC_DIRSHIFT) | \
     ((type) << _IOC_TYPESHIFT) | \
     ((nr)   << _IOC_NRSHIFT) | \
     ((size) & _IOC_SIZEMASK))

#define _IO(type, nr)       _IOC(_IOC_NONE,  (type), (nr), 0)
#define _IOW(type, nr, size) _IOC(_IOC_WRITE, (type), (nr), sizeof(size))
#define _IOR(type, nr, size) _IOC(_IOC_READ,  (type), (nr), sizeof(size))
#define _IOWR(type, nr, size) _IOC(_IOC(_IOC_READ|_IOC_WRITE, (type), (nr), sizeof(size)))

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
#define DRM_IOCTL_VERSION_NR         DRM_IOWR(DRM_IOCTL_VERSION, drm_version_t)
#define DRM_IOCTL_GET_CAP_NR         DRM_IOWR(DRM_IOCTL_GET_CAP, drm_get_cap_t)
#define DRM_IOCTL_SET_CLIENT_CAP_NR  DRM_IOW(DRM_IOCTL_SET_CLIENT_CAP, drm_set_client_cap_t)
#define DRM_IOCTL_GEM_CLOSE_NR       DRM_IOW(DRM_IOCTL_GEM_CLOSE, drm_gem_close_t)
#define DRM_IOCTL_GEM_FLINK_NR       DRM_IOWR(DRM_IOCTL_GEM_FLINK, drm_gem_flink_t)
#define DRM_IOCTL_GEM_OPEN_NR        DRM_IOWR(DRM_IOCTL_GEM_OPEN, drm_gem_open_t)
#define DRM_IOCTL_MODESET_CTL_NR     DRM_IOW(DRM_IOCTL_MODESET_CTL, drm_modeset_ctl_t)

#define DRM_IOCTL_MODE_GETRESOURCES_NR    DRM_IOWR(DRM_IOCTL_MODE_GETRESOURCES, drm_mode_card_res_t)
#define DRM_IOCTL_MODE_GETCRTC_NR         DRM_IOWR(DRM_IOCTL_MODE_GETCRTC, drm_mode_crtc_t)
#define DRM_IOCTL_MODE_SETCRTC_NR         DRM_IOWR(DRM_IOCTL_MODE_SETCRTC, drm_mode_crtc_t)
#define DRM_IOCTL_MODE_GETENCODER_NR      DRM_IOWR(DRM_IOCTL_MODE_GETENCODER, drm_mode_get_encoder_t)
#define DRM_IOCTL_MODE_GETCONNECTOR_NR    DRM_IOWR(DRM_IOCTL_MODE_GETCONNECTOR, drm_mode_get_connector_t)
#define DRM_IOCTL_MODE_ADDFB_NR           DRM_IOWR(DRM_IOCTL_MODE_ADDFB, drm_mode_fb_cmd_t)
#define DRM_IOCTL_MODE_ADDFB2_NR          DRM_IOWR(DRM_IOCTL_MODE_ADDFB2, drm_mode_fb_cmd2_t)
#define DRM_IOCTL_MODE_RMFB_NR            DRM_IOWR(DRM_IOCTL_MODE_RMFB, drm_mode_rmfb_t)
#define DRM_IOCTL_MODE_DIRTYFB_NR         DRM_IOWR(DRM_IOCTL_MODE_DIRTYFB, drm_mode_fb_dirty_cmd_t)
#define DRM_IOCTL_MODE_PAGE_FLIP_NR       DRM_IOWR(DRM_IOCTL_MODE_PAGE_FLIP, drm_mode_crtc_page_flip_t)
#define DRM_IOCTL_MODE_CREATE_DUMB_NR     DRM_IOWR(DRM_IOCTL_MODE_CREATE_DUMB, drm_mode_create_dumb_t)
#define DRM_IOCTL_MODE_MAP_DUMB_NR        DRM_IOWR(DRM_IOCTL_MODE_MAP_DUMB, drm_mode_map_dumb_t)
#define DRM_IOCTL_MODE_DESTROY_DUMB_NR    DRM_IOWR(DRM_IOCTL_MODE_DESTROY_DUMB, drm_mode_destroy_dumb_t)
#define DRM_IOCTL_MODE_GETFB_NR           DRM_IOWR(DRM_IOCTL_MODE_GETFB, drm_mode_fb_cmd_t)
#define DRM_IOCTL_MODE_ATOMIC_NR          DRM_IOWR(DRM_IOCTL_MODE_ATOMIC, drm_mode_atomic_t)

/* DRM 版本信息 */
typedef struct {
    int      version_major;      /* 输出：主版本号 */
    int      version_minor;      /* 输出：次版本号 */
    int      version_patchlevel; /* 输出：补丁级别 */
    size_t   name_len;           /* 输入/输出：名称长度 */
    char    *name;               /* 输出：驱动名称 */
    size_t   date_len;           /* 输入/输出：日期长度 */
    char    *date;               /* 输出：日期字符串 */
    size_t   desc_len;           /* 输入/输出：描述长度 */
    char    *desc;               /* 输出：描述字符串 */
} drm_version_t;

/* DRM 能力查询 */
#define DRM_CAP_DUMB_BUFFER       0x1
#define DRM_CAP_VBLANK_HIGH_CRTC  0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH 0x3
#define DRM_CAP_DUMB_PREFER_SHADOW   0x4
#define DRM_CAP_PRIME             0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC 0x6
#define DRM_CAP_ASYNC_PAGE_FLIP   0x7

typedef struct {
    uint64_t capability;   /* 输入：查询的能力 */
    uint64_t value;        /* 输出：能力值 */
} drm_get_cap_t;

typedef struct {
    uint64_t capability;   /* 输入：设置的能力 */
    uint64_t value;        /* 输入：能力值 */
} drm_set_client_cap_t;

/* Modeset 控制 */
typedef struct {
    uint32_t crtc;         /* CRTC ID */
    uint32_t cmd;          /* 命令 */
} drm_modeset_ctl_t;

/* GEM 操作 */
typedef struct {
    uint32_t handle;       /* 输入：要关闭的 GEM 句柄 */
    uint32_t pad;
} drm_gem_close_t;

typedef struct {
    uint32_t handle;       /* 输入：GEM 句柄 */
    uint32_t name;         /* 输出：全局名称 */
} drm_gem_flink_t;

typedef struct {
    uint32_t name;         /* 输入：全局名称 */
    uint32_t handle;       /* 输出：GEM 句柄 */
    uint64_t size;         /* 输出：缓冲区大小 */
} drm_gem_open_t;

/* DRM 事件类型 */
#define DRM_EVENT_VBLANK 0x01
#define DRM_EVENT_FLIP_COMPLETE 0x02

typedef struct {
    uint32_t type;         /* 事件类型 */
    uint32_t length;       /* 事件数据长度 */
} drm_event_t;

typedef struct {
    drm_event_t base;
    uint64_t user_data;    /* 用户数据 */
    uint32_t tv_sec;       /* 秒 */
    uint32_t tv_usec;      /* 微秒 */
    uint32_t sequence;     /* 序列号 */
    uint32_t crtc_id;      /* CRTC ID */
} drm_event_vblank_t;

/* DRM 设备能力 */
#define DRM_MASTER      0x01    /* 拥有 DRM-Master 权限 */
#define DRM_ROOT_ONLY   0x02    /* 仅 root 可访问 */

#endif /* DRM_H */
