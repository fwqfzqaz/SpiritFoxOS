/* ========================================================================
 * SpiritFoxOS Wayland 测试客户端
 *
 * 连接到 Wayland 合成器，创建一个窗口并渲染彩色渐变图案。
 * 验证：wl_display 连接、wl_shm 共享内存、wl_surface/xdg_toplevel 窗口管理、
 *       帧回调动画循环。
 * ======================================================================== */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>

#include <wayland-client.h>
#include "wayland-client-protocol.h"
#include "xdg-shell-client-protocol.h"

/* ------------------------------------------------------------------ *
 * 全局状态
 * ------------------------------------------------------------------ */

struct wl_display    *display;
struct wl_registry   *registry;
struct wl_compositor *compositor;
struct wl_shm        *shm;
struct xdg_wm_base   *xdg_wm_base;
struct wl_seat       *seat;
struct wl_output     *output;

struct wl_surface    *surface;
struct xdg_surface   *xdg_surface;
struct xdg_toplevel  *xdg_toplevel;
struct wl_callback   *frame_callback;

int display_width  = 1024;
int display_height = 768;
int window_width   = 400;
int window_height  = 300;

int configured = 0;
int running    = 1;

/* ------------------------------------------------------------------ *
 * 共享内存辅助
 * ------------------------------------------------------------------ */

static int
create_shm_file(size_t size)
{
    int fd = memfd_create("wayland-shm", 0);
    if (fd < 0) {
        perror("memfd_create");
        return -1;
    }
    if (ftruncate(fd, size) < 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ *
 * 渲染：绘制彩色渐变 + 移动方块
 * ------------------------------------------------------------------ */

static int frame_count = 0;

/* 前向声明 */
static const struct wl_callback_listener frame_callback_listener;

static void
render_frame(uint32_t *pixels, int width, int height, int frame)
{
    /* 背景渐变：蓝色 → 黑色 */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t r = (x * 255) / width;
            uint32_t g = (y * 255) / height;
            uint32_t b = 128 + (frame * 3) % 128;
            pixels[y * width + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }

    /* 移动方块（红色） */
    int bx = (frame * 3) % (width - 60);
    int by = (frame * 2) % (height - 60);
    for (int y = 0; y < 60; y++) {
        for (int x = 0; x < 60; x++) {
            pixels[(by + y) * width + (bx + x)] = 0xFFFF0000;
        }
    }

    /* 边框（白色） */
    for (int x = 0; x < width; x++) {
        pixels[x] = 0xFFFFFFFF;
        pixels[(height - 1) * width + x] = 0xFFFFFFFF;
    }
    for (int y = 0; y < height; y++) {
        pixels[y * width] = 0xFFFFFFFF;
        pixels[y * width + width - 1] = 0xFFFFFFFF;
    }
}

/* ------------------------------------------------------------------ *
 * 帧回调
 * ------------------------------------------------------------------ */

static void
frame_callback_handler(void *data, struct wl_callback *callback, uint32_t time)
{
    (void)data;
    (void)time;

    wl_callback_destroy(callback);
    frame_callback = NULL;

    if (!configured || !running)
        return;

    /* 创建新 SHM 缓冲区 */
    int stride = window_width * 4;
    int size = stride * window_height;
    int fd = create_shm_file(size);
    if (fd < 0)
        return;

    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return;
    }

    /* 渲染 */
    render_frame(pixels, window_width, window_height, frame_count++);
    if (frame_count == 10)
        fprintf(stderr, "[client] frame 10 rendered\n");

    /* 创建 wl_shm_pool 和 wl_buffer */
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
                                    window_width, window_height,
                                    stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    /* 附加缓冲区并提交 */
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, window_width, window_height);

    /* 请求下一帧回调 */
    frame_callback = wl_surface_frame(surface);
    wl_callback_add_listener(frame_callback, &frame_callback_listener, NULL);

    wl_surface_commit(surface);

    munmap(pixels, size);
    wl_buffer_destroy(buffer);
}

static const struct wl_callback_listener frame_callback_listener = {
    .done = frame_callback_handler,
};

/* ------------------------------------------------------------------ *
 * xdg_surface 监听器
 * ------------------------------------------------------------------ */

static void
xdg_surface_configure(void *data, struct xdg_surface *surf,
                      uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(surf, serial);

    if (!configured) {
        configured = 1;
        /* 触发第一帧 */
        frame_callback = wl_surface_frame(surface);
        wl_callback_add_listener(frame_callback, &frame_callback_listener, NULL);
        wl_surface_commit(surface);
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

/* ------------------------------------------------------------------ *
 * xdg_toplevel 监听器
 * ------------------------------------------------------------------ */

static void
xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                       int32_t width, int32_t height,
                       struct wl_array *states)
{
    (void)data;
    (void)toplevel;
    (void)states;

    if (width > 0 && height > 0) {
        window_width = width;
        window_height = height;
    }
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)data;
    (void)toplevel;
    running = 0;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

/* ------------------------------------------------------------------ *
 * xdg_wm_base 监听器
 * ------------------------------------------------------------------ */

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

/* ------------------------------------------------------------------ *
 * wl_registry 监听器
 * ------------------------------------------------------------------ */

static void
registry_global(void *data, struct wl_registry *reg,
                uint32_t name, const char *interface, uint32_t version)
{
    (void)data;
    (void)version;

    if (strcmp(interface, "wl_compositor") == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
        fprintf(stderr, "[client] bound wl_compositor\n");
    } else if (strcmp(interface, "wl_shm") == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
        fprintf(stderr, "[client] bound wl_shm\n");
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        xdg_wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);
        fprintf(stderr, "[client] bound xdg_wm_base\n");
    } else if (strcmp(interface, "wl_seat") == 0) {
        seat = wl_registry_bind(reg, name, &wl_seat_interface, 4);
        fprintf(stderr, "[client] bound wl_seat\n");
    } else if (strcmp(interface, "wl_output") == 0) {
        output = wl_registry_bind(reg, name, &wl_output_interface, 2);
        fprintf(stderr, "[client] bound wl_output\n");
    }
}

static void
registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Earliest possible output to verify main() is reached */
    {
        const char *_m = "[CL] main() entered\n";
        write(2, _m, strlen(_m));
    }

    fprintf(stderr, "[client] SpiritFoxOS Wayland test client\n");

    /* 1. 连接到 Wayland 显示 */
    {
        const char *_m = "[CL] calling wl_display_connect\n";
        write(2, _m, strlen(_m));
    }
    display = wl_display_connect(NULL);
    if (!display) {
        const char *_m = "[CL] wl_display_connect FAILED, retrying in 1s...\n";
        write(2, _m, strlen(_m));
        sleep(1);
        display = wl_display_connect(NULL);
    }
    if (!display) {
        fprintf(stderr, "[client] ERROR: cannot connect to Wayland display\n");
        return 1;
    }
    fprintf(stderr, "[client] Connected to Wayland display\n");

    /* 2. 获取 registry 并绑定全局对象 */
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !xdg_wm_base) {
        fprintf(stderr, "[client] ERROR: missing required globals\n");
        return 1;
    }

    /* 3. 创建 surface 和 xdg_surface */
    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(xdg_toplevel, "SpiritFoxOS Test");
    xdg_toplevel_set_app_id(xdg_toplevel, "spiritfoxos.test");

    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    fprintf(stderr, "[client] Window created (%dx%d)\n",
            window_width, window_height);

    /* 4. 主循环 */
    while (running) {
        if (wl_display_dispatch(display) < 0) {
            if (errno != EAGAIN && errno != EINTR) {
                fprintf(stderr, "[client] wl_display_dispatch error: %s\n",
                        strerror(errno));
                break;
            }
        }
    }

    /* 5. 清理 */
    fprintf(stderr, "[client] Cleaning up\n");
    if (frame_callback)
        wl_callback_destroy(frame_callback);
    if (xdg_toplevel)
        xdg_toplevel_destroy(xdg_toplevel);
    if (xdg_surface)
        xdg_surface_destroy(xdg_surface);
    if (surface)
        wl_surface_destroy(surface);
    if (xdg_wm_base)
        xdg_wm_base_destroy(xdg_wm_base);
    if (shm)
        wl_shm_destroy(shm);
    if (compositor)
        wl_compositor_destroy(compositor);
    if (registry)
        wl_registry_destroy(registry);
    wl_display_disconnect(display);

    fprintf(stderr, "[client] Done (rendered %d frames)\n", frame_count);
    return 0;
}
