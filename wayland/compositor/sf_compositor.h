#ifndef SF_COMPOSITOR_H
#define SF_COMPOSITOR_H

#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include <xdg-shell-server-protocol.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <linux/input.h>
#include <linux/drm.h>

/* ========================================================================
 * SpiritFoxOS Wayland Compositor
 * 最小化 Wayland 合成器，使用 DRM/KMS 显示和 evdev 输入
 * ======================================================================== */

#define MAX_SURFACES  64
#define MAX_CLIENTS   16
#define MAX_INPUT_DEV 8

/* 服务器端装饰常量 */
#define SF_TITLE_BAR_HEIGHT  28
#define SF_BORDER_WIDTH      2
#define SF_CLOSE_BUTTON_SIZE 20
#define SF_CLOSE_BUTTON_PAD  4

/* 桌面面板（任务栏） */
#define SF_PANEL_HEIGHT      32

/* 前向声明 */
typedef struct sf_compositor sf_compositor_t;
typedef struct sf_surface    sf_surface_t;
typedef struct sf_output     sf_output_t;
typedef struct sf_seat       sf_seat_t;
typedef struct sf_client     sf_client_t;

/* DRM 帧缓冲 */
typedef struct {
    uint32_t fb_id;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
    void    *map;       /* mmap 映射地址 */
    uint32_t width;
    uint32_t height;
} sf_dumb_fb_t;

/* 输出（显示器） */
struct sf_output {
    sf_compositor_t *comp;
    struct wl_global *global;
    struct wl_resource *resource;

    /* DRM */
    int drm_fd;
    uint32_t crtc_id;
    uint32_t connector_id;
    uint32_t encoder_id;
    struct drm_mode_modeinfo mode;
    void *connector;  /* drmModeConnector* (opaque) */

    /* 帧缓冲（双缓冲） */
    sf_dumb_fb_t fb[2];
    int current_fb;
    int page_flip_pending;

    /* 显示信息 */
    int width;
    int height;
    int refresh_rate;
    int32_t mm_width;
    int32_t mm_height;
};

/* Surface */
struct sf_surface {
    struct wl_resource *resource;
    sf_compositor_t *comp;

    /* 当前缓冲区 */
    struct wl_resource *buffer_resource;
    struct wl_listener pending_buffer_destroy;
    int buffer_pending;
    void *buffer_data;      /* 服务器拥有的已提交像素快照 */
    int32_t buffer_width;
    int32_t buffer_height;
    uint32_t buffer_format;
    int32_t buffer_stride;

    /* 位置和大小 */
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;

    /* 状态 */
    int dirty;
    int mapped;

    /* xdg_shell */
    struct wl_resource *xdg_surface;
    struct wl_resource *xdg_toplevel;
    char title[256];

    /* 窗口管理状态 */
    int is_maximized;
    int is_fullscreen;
    int is_minimized;

    /* 交互状态 */
    int dragging;          /* 正在拖动窗口 */
    int resizing;          /* 正在缩放窗口 */
    uint32_t resize_edges; /* 缩放边 */
    int32_t drag_offset_x; /* 拖动偏移 */
    int32_t drag_offset_y;
    int32_t resize_start_w; /* 缩放开始时窗口尺寸 */
    int32_t resize_start_h;
    int32_t resize_start_x;
    int32_t resize_start_y;
    int32_t resize_start_ptr_x;
    int32_t resize_start_ptr_y;

    /* 保存最大化前的窗口位置 */
    int32_t saved_x;
    int32_t saved_y;
    int32_t saved_width;
    int32_t saved_height;

    /* 帧回调 */
    struct wl_list frame_callbacks;

    /* 链表 */
    struct wl_list link;
};

/* 座位（输入设备） */
struct sf_seat {
    sf_compositor_t *comp;
    struct wl_global *global;
    struct wl_resource *seat_resource;
    struct wl_resource *keyboard_resource;
    struct wl_resource *pointer_resource;

    /* 键盘焦点 */
    sf_surface_t *keyboard_focus;
    uint32_t keyboard_modifiers;

    /* 指针 */
    sf_surface_t *pointer_focus;
    int32_t pointer_x;
    int32_t pointer_y;

    /* 光标 */
    struct wl_resource *cursor_surface;
    int32_t cursor_hotspot_x;
    int32_t cursor_hotspot_y;
    int cursor_visible;

    /* evdev 设备 */
    int keyboard_fd;
    int pointer_fd;
};

/* 客户端 */
struct sf_client {
    struct wl_client *client;
    struct wl_list surfaces;
};

/* 合成器主结构 */
struct sf_compositor {
    struct wl_display *display;
    struct wl_event_loop *event_loop;

    /* 全局对象 */
    struct wl_global *compositor_global;
    struct wl_global *shm_global;
    struct wl_global *output_global;
    struct wl_global *seat_global;
    struct wl_global *xdg_wm_base_global;

    /* 输出 */
    sf_output_t output;

    /* 座位 */
    sf_seat_t seat;

    /* Surface 列表 */
    struct wl_list surfaces;

    /* 事件循环 */
    int epoll_fd;
    struct wl_event_source *drm_event_source;
    struct wl_event_source *keyboard_event_source;
    struct wl_event_source *pointer_event_source;

    /* 运行状态 */
    int running;
    int need_composite;  /* 需要重新合成 */

    /* 背景缓存（预渲染，避免每帧重算） */
    uint32_t *bg_cache;
};

/* ========================================================================
 * API
 * ======================================================================== */

int sf_compositor_init(sf_compositor_t *comp);
void sf_compositor_run(sf_compositor_t *comp);
void sf_compositor_finish(sf_compositor_t *comp);

/* 输出管理 */
int sf_output_init(sf_output_t *output, sf_compositor_t *comp);
void sf_output_render(sf_output_t *output, sf_compositor_t *comp);
void sf_output_finish(sf_output_t *output);

/* 输入管理 */
int sf_seat_init(sf_seat_t *seat, sf_compositor_t *comp);
void sf_seat_handle_keyboard_event(sf_seat_t *seat, sf_compositor_t *comp);
void sf_seat_handle_pointer_event(sf_seat_t *seat, sf_compositor_t *comp);
void sf_seat_finish(sf_seat_t *seat);

/* Surface 管理 */
sf_surface_t *sf_surface_create(sf_compositor_t *comp, struct wl_client *client,
                                 uint32_t id);
void sf_surface_destroy(sf_surface_t *surface);
void sf_surface_attach(sf_surface_t *surface, struct wl_resource *buffer,
                       int32_t x, int32_t y);
void sf_surface_commit(sf_surface_t *surface);

/* 合成 */
void sf_compositor_composite(sf_compositor_t *comp);

#endif /* SF_COMPOSITOR_H */
