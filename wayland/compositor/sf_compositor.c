/* ========================================================================
 * SpiritFoxOS Wayland Compositor - 主实现
 *
 * 最小化 Wayland 合成器：
 *   - libwayland-server 协议回调
 *   - DRM/KMS 输出 (/dev/dri/card0, dumb 帧缓冲 + 双缓冲 + page flip)
 *   - evdev 输入 (/dev/input/event0 键盘, /dev/input/event1 鼠标)
 *   - CPU 软件合成 (XRGB8888 / ARGB8888, 含 alpha 混合)
 * ======================================================================== */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <linux/drm.h>
#include <linux/input.h>

/* -----------------------------------------------------------------------
 * libdrm 兼容类型：本 sysroot 的 <linux/drm.h> 使用内核风格的小写结构体名
 * (struct drm_mode_modeinfo / struct drm_mode_get_connector)，而 sf_compositor.h
 * 使用 libdrm 风格的 drmModeModeInfo / drmModeConnector。此处定义兼容别名，使
 * 头文件中的字段类型可解析。必须在包含 sf_compositor.h 之前定义。
 * --------------------------------------------------------------------- */
typedef struct drm_mode_modeinfo drmModeModeInfo;

typedef struct drmModeConnector {
	uint32_t            connector_id;
	uint32_t            connector_type;
	uint32_t            connector_type_id;
	uint32_t            connection;
	uint32_t            mm_width;
	uint32_t            mm_height;
	uint32_t            subpixel;
	uint32_t            encoder_id;
	uint32_t            count_modes;
	drmModeModeInfo    *modes;
	uint32_t            count_encoders;
	uint32_t           *encoders;
} drmModeConnector;

#include "sf_compositor.h"

/* ========================================================================
 * 内部常量与数据结构
 * ======================================================================== */

/* wl_keyboard 修饰键掩码 (与 xkbcommon 标准一致) */
#define SF_MOD_SHIFT  0x01
#define SF_MOD_CAPS   0x02
#define SF_MOD_CTRL   0x04
#define SF_MOD_ALT    0x08
#define SF_MOD_LOGO   0x10

/* 协议版本 */
#define SF_COMPOSITOR_VERSION 4
#define SF_SHM_VERSION        1
#define SF_OUTPUT_VERSION     2
#define SF_SEAT_VERSION       5
#define SF_XDG_WM_BASE_VERSION 1

/* SHM 池：客户端通过 wl_shm.create_pool 传递的 fd 被 mmap 到本地 */
typedef struct sf_shm_pool {
	void  *data;        /* mmap 映射地址 */
	size_t size;        /* 当前映射大小 */
	int    fd;          /* 原始文件描述符 (resize 时重新 mmap) */
	int    refcount;    /* 引用计数 (池自身 + 每个派生 buffer) */
} sf_shm_pool_t;

/* SHM buffer：从池中偏移截取的像素数据 */
typedef struct sf_buffer {
	sf_shm_pool_t *pool;
	void          *data;     /* pool->data + offset */
	int32_t        width;
	int32_t        height;
	int32_t        stride;
	uint32_t       format;
} sf_buffer_t;

/* 帧回调 */
typedef struct sf_frame_callback {
	struct wl_resource *resource;
	struct wl_list      link;
} sf_frame_callback_t;

/* ========================================================================
 * 前向声明
 * ======================================================================== */
void sf_compositor_composite(sf_compositor_t *comp);
static void sf_seat_set_keyboard_focus(sf_seat_t *seat, sf_compositor_t *comp,
                                       sf_surface_t *surface);
static void sf_seat_set_pointer_focus(sf_seat_t *seat, sf_compositor_t *comp,
                                      sf_surface_t *surface);
static sf_surface_t *sf_surface_at(sf_compositor_t *comp, int32_t x, int32_t y);
static int  sf_dumb_fb_create(sf_output_t *output, sf_dumb_fb_t *fb,
                              uint32_t width, uint32_t height);
static void sf_dumb_fb_destroy(sf_output_t *output, sf_dumb_fb_t *fb);
static int  sf_drm_event_handler(int fd, uint32_t mask, void *data);

static uint32_t sf_time_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

/* ========================================================================
 * wl_surface 接口实现
 * ======================================================================== */

static void sf_surface_destroy_impl(struct wl_client *client,
                                    struct wl_resource *resource);
static void sf_surface_attach_impl(struct wl_client *client,
                                   struct wl_resource *resource,
                                   struct wl_resource *buffer, int32_t x, int32_t y);
static void sf_surface_damage_impl(struct wl_client *client,
                                   struct wl_resource *resource,
                                   int32_t x, int32_t y, int32_t width, int32_t height);
static void sf_surface_frame_impl(struct wl_client *client,
                                  struct wl_resource *resource, uint32_t callback);
static void sf_surface_set_opaque_region_impl(struct wl_client *client,
                                              struct wl_resource *resource,
                                              struct wl_resource *region);
static void sf_surface_set_input_region_impl(struct wl_client *client,
                                             struct wl_resource *resource,
                                             struct wl_resource *region);
static void sf_surface_commit_impl(struct wl_client *client,
                                   struct wl_resource *resource);
static void sf_surface_set_buffer_transform_impl(struct wl_client *client,
                                                 struct wl_resource *resource,
                                                 int32_t transform);
static void sf_surface_set_buffer_scale_impl(struct wl_client *client,
                                             struct wl_resource *resource,
                                             int32_t scale);
static void sf_surface_damage_buffer_impl(struct wl_client *client,
                                          struct wl_resource *resource,
                                          int32_t x, int32_t y,
                                          int32_t width, int32_t height);
static void sf_surface_offset_impl(struct wl_client *client,
                                   struct wl_resource *resource,
                                   int32_t x, int32_t y);

static const struct wl_surface_interface sf_surface_impl = {
	.destroy              = sf_surface_destroy_impl,
	.attach               = sf_surface_attach_impl,
	.damage               = sf_surface_damage_impl,
	.frame                = sf_surface_frame_impl,
	.set_opaque_region    = sf_surface_set_opaque_region_impl,
	.set_input_region     = sf_surface_set_input_region_impl,
	.commit               = sf_surface_commit_impl,
	.set_buffer_transform = sf_surface_set_buffer_transform_impl,
	.set_buffer_scale     = sf_surface_set_buffer_scale_impl,
	.damage_buffer        = sf_surface_damage_buffer_impl,
	.offset               = sf_surface_offset_impl,
};

static void sf_surface_resource_destroy(struct wl_resource *resource)
{
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	sf_compositor_t *comp = surface->comp;

	if (comp->seat.keyboard_focus == surface)
		sf_seat_set_keyboard_focus(&comp->seat, comp, NULL);
	if (comp->seat.pointer_focus == surface)
		sf_seat_set_pointer_focus(&comp->seat, comp, NULL);

	sf_frame_callback_t *cb, *tmp;
	wl_list_for_each_safe(cb, tmp, &surface->frame_callbacks, link) {
		wl_resource_destroy(cb->resource);
		wl_list_remove(&cb->link);
		free(cb);
	}

	wl_list_remove(&surface->link);
	free(surface);
}

static void sf_surface_destroy_impl(struct wl_client *client,
                                    struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void sf_surface_attach_impl(struct wl_client *client,
                                   struct wl_resource *resource,
                                   struct wl_resource *buffer, int32_t x, int32_t y)
{
	(void)client;
	(void)x;
	(void)y;
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	/* 暂存为 pending buffer，commit 时解析 */
	surface->buffer_resource = buffer;
}

static void sf_surface_damage_impl(struct wl_client *client,
                                   struct wl_resource *resource,
                                   int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	surface->dirty = 1;
}

static void sf_surface_frame_impl(struct wl_client *client,
                                  struct wl_resource *resource, uint32_t callback)
{
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	struct wl_resource *cb_res = wl_resource_create(client,
		&wl_callback_interface, 1, callback);
	if (!cb_res) {
		wl_client_post_no_memory(client);
		return;
	}
	sf_frame_callback_t *cb = calloc(1, sizeof(*cb));
	if (!cb) {
		wl_resource_destroy(cb_res);
		wl_client_post_no_memory(client);
		return;
	}
	cb->resource = cb_res;
	wl_list_insert(surface->frame_callbacks.prev, &cb->link);
}

static void sf_surface_set_opaque_region_impl(struct wl_client *client,
                                              struct wl_resource *resource,
                                              struct wl_resource *region)
{
	(void)client;
	(void)resource;
	(void)region;
}

static void sf_surface_set_input_region_impl(struct wl_client *client,
                                             struct wl_resource *resource,
                                             struct wl_resource *region)
{
	(void)client;
	(void)resource;
	(void)region;
}

void sf_surface_commit(sf_surface_t *surface)
{
	sf_compositor_t *comp = surface->comp;
	struct wl_resource *bres = surface->buffer_resource;

	if (bres) {
		sf_buffer_t *buf = wl_resource_get_user_data(bres);
		if (buf) {
			int was_mapped = surface->mapped;
			surface->buffer_data   = buf->data;
			surface->buffer_width  = buf->width;
			surface->buffer_height = buf->height;
			surface->buffer_format = buf->format;
			surface->buffer_stride = buf->stride;
			surface->width         = buf->width;
			surface->height        = buf->height;
			surface->mapped        = 1;
			/* 首次映射时居中放置 */
			if (!was_mapped) {
				surface->x = (comp->output.width - buf->width) / 2;
				surface->y = (comp->output.height - buf->height) / 2;
				if (surface->x < 0) surface->x = 0;
				if (surface->y < 0) surface->y = 0;
				/* 新映射的表面获得键盘焦点 */
				sf_seat_set_keyboard_focus(&comp->seat, comp, surface);
			}
		}
	} else {
		/* attach(NULL) 或无缓冲区提交：取消映射 */
		surface->buffer_data = NULL;
		if (surface->mapped) {
			surface->mapped = 0;
			if (comp->seat.keyboard_focus == surface)
				sf_seat_set_keyboard_focus(&comp->seat, comp, NULL);
		}
	}
	surface->dirty = 1;
}

static void sf_surface_commit_impl(struct wl_client *client,
                                   struct wl_resource *resource)
{
	(void)client;
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	sf_surface_commit(surface);
	sf_compositor_composite(surface->comp);
}

static void sf_surface_set_buffer_transform_impl(struct wl_client *client,
                                                 struct wl_resource *resource,
                                                 int32_t transform)
{
	(void)client;
	(void)resource;
	(void)transform;
}

static void sf_surface_set_buffer_scale_impl(struct wl_client *client,
                                             struct wl_resource *resource,
                                             int32_t scale)
{
	(void)client;
	(void)resource;
	(void)scale;
}

static void sf_surface_damage_buffer_impl(struct wl_client *client,
                                          struct wl_resource *resource,
                                          int32_t x, int32_t y,
                                          int32_t width, int32_t height)
{
	(void)client;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	surface->dirty = 1;
}

static void sf_surface_offset_impl(struct wl_client *client,
                                   struct wl_resource *resource,
                                   int32_t x, int32_t y)
{
	(void)client;
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	surface->x += x;
	surface->y += y;
}

/* ========================================================================
 * wl_region 接口实现
 * ======================================================================== */

static void sf_region_destroy_impl(struct wl_client *client,
                                   struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}
static void sf_region_add_impl(struct wl_client *client,
                               struct wl_resource *resource,
                               int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}
static void sf_region_subtract_impl(struct wl_client *client,
                                    struct wl_resource *resource,
                                    int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static const struct wl_region_interface sf_region_impl = {
	.destroy  = sf_region_destroy_impl,
	.add      = sf_region_add_impl,
	.subtract = sf_region_subtract_impl,
};

/* ========================================================================
 * wl_compositor 接口实现
 * ======================================================================== */

static void sf_compositor_create_surface_impl(struct wl_client *client,
                                              struct wl_resource *resource,
                                              uint32_t id)
{
	sf_compositor_t *comp = wl_resource_get_user_data(resource);
	sf_surface_create(comp, client, id);
}

static void sf_compositor_create_region_impl(struct wl_client *client,
                                             struct wl_resource *resource,
                                             uint32_t id)
{
	(void)resource;
	struct wl_resource *r = wl_resource_create(client, &wl_region_interface, 1, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &sf_region_impl, NULL, NULL);
}

static const struct wl_compositor_interface sf_compositor_impl = {
	.create_surface = sf_compositor_create_surface_impl,
	.create_region  = sf_compositor_create_region_impl,
};

static void sf_compositor_bind(struct wl_client *client, void *data,
                               uint32_t version, uint32_t id)
{
	sf_compositor_t *comp = data;
	struct wl_resource *r = wl_resource_create(client, &wl_compositor_interface,
	                                           version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &sf_compositor_impl, comp, NULL);
}

/* ========================================================================
 * wl_shm / wl_shm_pool / wl_buffer 接口实现
 * ======================================================================== */

static void sf_shm_pool_resource_destroy(struct wl_resource *resource);

static void sf_shm_create_buffer_impl(struct wl_client *client,
                                      struct wl_resource *resource, uint32_t id,
                                      int32_t offset, int32_t width, int32_t height,
                                      int32_t stride, uint32_t format);
static void sf_shm_pool_destroy_impl(struct wl_client *client,
                                     struct wl_resource *resource);
static void sf_shm_pool_resize_impl(struct wl_client *client,
                                    struct wl_resource *resource, int32_t size);

static const struct wl_shm_pool_interface sf_shm_pool_impl = {
	.create_buffer = sf_shm_create_buffer_impl,
	.destroy       = sf_shm_pool_destroy_impl,
	.resize        = sf_shm_pool_resize_impl,
};

static void sf_buffer_destroy_impl(struct wl_client *client,
                                   struct wl_resource *resource);
static const struct wl_buffer_interface sf_buffer_impl = {
	.destroy = sf_buffer_destroy_impl,
};

static void sf_shm_create_pool_impl(struct wl_client *client,
                                    struct wl_resource *resource, uint32_t id,
                                    int32_t fd, int32_t size)
{
	(void)resource;
	void *data = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		wl_client_post_no_memory(client);
		return;
	}
	sf_shm_pool_t *pool = calloc(1, sizeof(*pool));
	if (!pool) {
		munmap(data, (size_t)size);
		close(fd);
		wl_client_post_no_memory(client);
		return;
	}
	pool->data = data;
	pool->size = (size_t)size;
	pool->fd = fd;
	pool->refcount = 1;

	struct wl_resource *r = wl_resource_create(client, &wl_shm_pool_interface,
	                                           SF_SHM_VERSION, id);
	if (!r) {
		munmap(data, (size_t)size);
		close(fd);
		free(pool);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &sf_shm_pool_impl, pool,
	                               sf_shm_pool_resource_destroy);
}

static const struct wl_shm_interface sf_shm_impl = {
	.create_pool = sf_shm_create_pool_impl,
	.release     = NULL,
};

static void sf_shm_bind(struct wl_client *client, void *data,
                        uint32_t version, uint32_t id)
{
	sf_compositor_t *comp = data;
	struct wl_resource *r = wl_resource_create(client, &wl_shm_interface,
	                                           version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &sf_shm_impl, comp, NULL);
	/* 广告支持的像素格式 */
	wl_shm_send_format(r, WL_SHM_FORMAT_ARGB8888);
	wl_shm_send_format(r, WL_SHM_FORMAT_XRGB8888);
}

static void sf_shm_pool_resource_destroy(struct wl_resource *resource)
{
	sf_shm_pool_t *pool = wl_resource_get_user_data(resource);
	pool->refcount--;
	if (pool->refcount <= 0) {
		if (pool->data && pool->data != MAP_FAILED)
			munmap(pool->data, pool->size);
		if (pool->fd >= 0)
			close(pool->fd);
		free(pool);
	}
}

static void sf_shm_pool_destroy_impl(struct wl_client *client,
                                     struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void sf_shm_pool_resize_impl(struct wl_client *client,
                                    struct wl_resource *resource, int32_t size)
{
	(void)client;
	sf_shm_pool_t *pool = wl_resource_get_user_data(resource);
	if (size <= 0 || (size_t)size <= pool->size)
		return;
	/* 重新 mmap 扩展映射 */
	void *new_data = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
	                      MAP_SHARED, pool->fd, 0);
	if (new_data == MAP_FAILED)
		return;
	if (pool->data && pool->data != MAP_FAILED)
		munmap(pool->data, pool->size);
	pool->data = new_data;
	pool->size = (size_t)size;
}

static void sf_buffer_resource_destroy(struct wl_resource *resource)
{
	sf_buffer_t *buf = wl_resource_get_user_data(resource);
	if (buf) {
		sf_shm_pool_t *pool = buf->pool;
		free(buf);
		if (pool) {
			pool->refcount--;
			if (pool->refcount <= 0) {
				if (pool->data && pool->data != MAP_FAILED)
					munmap(pool->data, pool->size);
				if (pool->fd >= 0)
					close(pool->fd);
				free(pool);
			}
		}
	}
}

static void sf_shm_create_buffer_impl(struct wl_client *client,
                                      struct wl_resource *resource, uint32_t id,
                                      int32_t offset, int32_t width, int32_t height,
                                      int32_t stride, uint32_t format)
{
	sf_shm_pool_t *pool = wl_resource_get_user_data(resource);
	sf_buffer_t *buf = calloc(1, sizeof(*buf));
	if (!buf) {
		wl_client_post_no_memory(client);
		return;
	}
	buf->pool   = pool;
	buf->data   = (char *)pool->data + offset;
	buf->width  = width;
	buf->height = height;
	buf->stride = stride;
	buf->format = format;
	pool->refcount++;

	struct wl_resource *r = wl_resource_create(client, &wl_buffer_interface, 1, id);
	if (!r) {
		free(buf);
		pool->refcount--;
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &sf_buffer_impl, buf,
	                               sf_buffer_resource_destroy);
}

static void sf_buffer_destroy_impl(struct wl_client *client,
                                   struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

/* ========================================================================
 * wl_output 接口实现
 * ======================================================================== */

static void sf_output_release_impl(struct wl_client *client,
                                   struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_output_interface sf_output_impl = {
	.release = sf_output_release_impl,
};

static void sf_output_bind(struct wl_client *client, void *data,
                           uint32_t version, uint32_t id)
{
	sf_output_t *output = data;
	struct wl_resource *r = wl_resource_create(client, &wl_output_interface,
	                                           version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &sf_output_impl, output, NULL);
	output->resource = r;

	wl_output_send_geometry(r, 0, 0,
	                        output->mm_width,
	                        output->mm_height,
	                        WL_OUTPUT_SUBPIXEL_UNKNOWN,
	                        "SpiritFoxOS", "DRM", WL_OUTPUT_TRANSFORM_NORMAL);
	wl_output_send_mode(r,
	                    WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
	                    output->width, output->height,
	                    (int32_t)output->refresh_rate);
	if (version >= 2) {
		wl_output_send_scale(r, 1);
		wl_output_send_done(r);
	}
}

/* ========================================================================
 * wl_seat / wl_keyboard / wl_pointer / wl_touch 接口实现
 * ======================================================================== */

static void sf_keyboard_release_impl(struct wl_client *client,
                                     struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}
static const struct wl_keyboard_interface sf_keyboard_impl = {
	.release = sf_keyboard_release_impl,
};

static void sf_pointer_set_cursor_impl(struct wl_client *client,
                                       struct wl_resource *resource,
                                       uint32_t serial,
                                       struct wl_resource *surface,
                                       int32_t hotspot_x, int32_t hotspot_y)
{
	(void)client; (void)resource; (void)serial; (void)surface;
	(void)hotspot_x; (void)hotspot_y;
}
static void sf_pointer_release_impl(struct wl_client *client,
                                    struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}
static const struct wl_pointer_interface sf_pointer_impl = {
	.set_cursor = sf_pointer_set_cursor_impl,
	.release    = sf_pointer_release_impl,
};

static void sf_touch_release_impl(struct wl_client *client,
                                  struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}
static const struct wl_touch_interface sf_touch_impl = {
	.release = sf_touch_release_impl,
};

static void sf_seat_get_pointer_impl(struct wl_client *client,
                                     struct wl_resource *resource, uint32_t id)
{
	sf_compositor_t *comp = wl_resource_get_user_data(resource);
	struct wl_resource *r = wl_resource_create(client, &wl_pointer_interface,
	                                           wl_resource_get_version(resource), id);
	if (!r) { wl_client_post_no_memory(client); return; }
	wl_resource_set_implementation(r, &sf_pointer_impl, comp, NULL);
	comp->seat.pointer_resource = r;
	/* 若当前已有指针焦点，发送 enter */
	if (comp->seat.pointer_focus && comp->seat.pointer_focus->resource) {
		uint32_t serial = wl_display_next_serial(comp->display);
		wl_pointer_send_enter(r, serial, comp->seat.pointer_focus->resource,
			wl_fixed_from_int(comp->seat.pointer_x - comp->seat.pointer_focus->x),
			wl_fixed_from_int(comp->seat.pointer_y - comp->seat.pointer_focus->y));
	}
}

static void sf_seat_get_keyboard_impl(struct wl_client *client,
                                      struct wl_resource *resource, uint32_t id)
{
	sf_compositor_t *comp = wl_resource_get_user_data(resource);
	struct wl_resource *r = wl_resource_create(client, &wl_keyboard_interface,
	                                           wl_resource_get_version(resource), id);
	if (!r) { wl_client_post_no_memory(client); return; }
	wl_resource_set_implementation(r, &sf_keyboard_impl, comp, NULL);
	comp->seat.keyboard_resource = r;

	/* 发送空 keymap (NO_KEYMAP) */
	int pipefd[2];
	if (pipe(pipefd) == 0) {
		wl_keyboard_send_keymap(r, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP,
		                        pipefd[0], 0);
		close(pipefd[0]);
		close(pipefd[1]);
	}
	/* 若当前已有键盘焦点，发送 enter */
	if (comp->seat.keyboard_focus && comp->seat.keyboard_focus->resource) {
		uint32_t serial = wl_display_next_serial(comp->display);
		struct wl_array keys;
		wl_array_init(&keys);
		wl_keyboard_send_enter(r, serial, comp->seat.keyboard_focus->resource, &keys);
		wl_array_release(&keys);
	}
}

static void sf_seat_get_touch_impl(struct wl_client *client,
                                   struct wl_resource *resource, uint32_t id)
{
	(void)resource;
	struct wl_resource *r = wl_resource_create(client, &wl_touch_interface, 1, id);
	if (!r) { wl_client_post_no_memory(client); return; }
	wl_resource_set_implementation(r, &sf_touch_impl, NULL, NULL);
}

static void sf_seat_release_impl(struct wl_client *client,
                                 struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_seat_interface sf_seat_impl = {
	.get_pointer = sf_seat_get_pointer_impl,
	.get_keyboard = sf_seat_get_keyboard_impl,
	.get_touch   = sf_seat_get_touch_impl,
	.release     = sf_seat_release_impl,
};

static void sf_seat_bind(struct wl_client *client, void *data,
                         uint32_t version, uint32_t id)
{
	sf_compositor_t *comp = data;
	struct wl_resource *r = wl_resource_create(client, &wl_seat_interface,
	                                           version, id);
	if (!r) { wl_client_post_no_memory(client); return; }
	wl_resource_set_implementation(r, &sf_seat_impl, comp, NULL);
	comp->seat.seat_resource = r;
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
	wl_seat_send_capabilities(r, caps);
	if (version >= 2)
		wl_seat_send_name(r, "seat0");
}

static void sf_seat_set_keyboard_focus(sf_seat_t *seat, sf_compositor_t *comp,
                                       sf_surface_t *surface)
{
	if (seat->keyboard_focus == surface)
		return;
	if (seat->keyboard_resource && seat->keyboard_focus &&
	    seat->keyboard_focus->resource) {
		uint32_t serial = wl_display_next_serial(comp->display);
		wl_keyboard_send_leave(seat->keyboard_resource, serial,
		                       seat->keyboard_focus->resource);
	}
	seat->keyboard_focus = surface;
	if (seat->keyboard_resource && surface && surface->resource) {
		uint32_t serial = wl_display_next_serial(comp->display);
		struct wl_array keys;
		wl_array_init(&keys);
		wl_keyboard_send_enter(seat->keyboard_resource, serial,
		                       surface->resource, &keys);
		wl_array_release(&keys);
	}
}

static void sf_seat_set_pointer_focus(sf_seat_t *seat, sf_compositor_t *comp,
                                      sf_surface_t *surface)
{
	if (seat->pointer_focus == surface)
		return;
	if (seat->pointer_resource && seat->pointer_focus &&
	    seat->pointer_focus->resource) {
		uint32_t serial = wl_display_next_serial(comp->display);
		wl_pointer_send_leave(seat->pointer_resource, serial,
		                      seat->pointer_focus->resource);
	}
	seat->pointer_focus = surface;
	if (seat->pointer_resource && surface && surface->resource) {
		uint32_t serial = wl_display_next_serial(comp->display);
		wl_pointer_send_enter(seat->pointer_resource, serial, surface->resource,
			wl_fixed_from_int(seat->pointer_x - surface->x),
			wl_fixed_from_int(seat->pointer_y - surface->y));
	}
}

static sf_surface_t *sf_surface_at(sf_compositor_t *comp, int32_t x, int32_t y)
{
	sf_surface_t *surface;
	/* 从顶层（链表尾部）向底层查找 */
	wl_list_for_each_reverse(surface, &comp->surfaces, link) {
		if (!surface->mapped || !surface->buffer_data)
			continue;
		if (x >= surface->x && x < surface->x + surface->width &&
		    y >= surface->y && y < surface->y + surface->height)
			return surface;
	}
	return NULL;
}

/* ========================================================================
 * xdg_shell 接口实现
 * ======================================================================== */

/* --- xdg_positioner --- */
static void sf_xdg_pos_destroy_impl(struct wl_client *client,
                                    struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}
static void sf_xdg_pos_set_size_impl(struct wl_client *client,
                                     struct wl_resource *resource,
                                     int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)width; (void)height;
}
static void sf_xdg_pos_set_anchor_rect_impl(struct wl_client *client,
                                            struct wl_resource *resource,
                                            int32_t x, int32_t y,
                                            int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}
static void sf_xdg_pos_set_anchor_impl(struct wl_client *client,
                                       struct wl_resource *resource, uint32_t anchor)
{
	(void)client; (void)resource; (void)anchor;
}
static void sf_xdg_pos_set_gravity_impl(struct wl_client *client,
                                        struct wl_resource *resource, uint32_t gravity)
{
	(void)client; (void)resource; (void)gravity;
}
static void sf_xdg_pos_set_constraint_adjustment_impl(struct wl_client *client,
                                                      struct wl_resource *resource,
                                                      uint32_t constraint_adjustment)
{
	(void)client; (void)resource; (void)constraint_adjustment;
}
static void sf_xdg_pos_set_offset_impl(struct wl_client *client,
                                       struct wl_resource *resource,
                                       int32_t x, int32_t y)
{
	(void)client; (void)resource; (void)x; (void)y;
}
static void sf_xdg_pos_set_reactive_impl(struct wl_client *client,
                                         struct wl_resource *resource)
{
	(void)client; (void)resource;
}
static void sf_xdg_pos_set_parent_size_impl(struct wl_client *client,
                                            struct wl_resource *resource,
                                            int32_t parent_width, int32_t parent_height)
{
	(void)client; (void)resource; (void)parent_width; (void)parent_height;
}
static void sf_xdg_pos_set_parent_configure_impl(struct wl_client *client,
                                                 struct wl_resource *resource,
                                                 uint32_t serial)
{
	(void)client; (void)resource; (void)serial;
}

static const struct xdg_positioner_interface sf_xdg_positioner_impl = {
	.destroy                 = sf_xdg_pos_destroy_impl,
	.set_size                = sf_xdg_pos_set_size_impl,
	.set_anchor_rect         = sf_xdg_pos_set_anchor_rect_impl,
	.set_anchor              = sf_xdg_pos_set_anchor_impl,
	.set_gravity             = sf_xdg_pos_set_gravity_impl,
	.set_constraint_adjustment = sf_xdg_pos_set_constraint_adjustment_impl,
	.set_offset              = sf_xdg_pos_set_offset_impl,
	.set_reactive            = sf_xdg_pos_set_reactive_impl,
	.set_parent_size         = sf_xdg_pos_set_parent_size_impl,
	.set_parent_configure    = sf_xdg_pos_set_parent_configure_impl,
};

/* --- xdg_popup --- */
static void sf_xdg_popup_destroy_impl(struct wl_client *client,
                                      struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}
static void sf_xdg_popup_grab_impl(struct wl_client *client,
                                   struct wl_resource *resource,
                                   struct wl_resource *seat, uint32_t serial)
{
	(void)client; (void)resource; (void)seat; (void)serial;
}
static void sf_xdg_popup_reposition_impl(struct wl_client *client,
                                         struct wl_resource *resource,
                                         struct wl_resource *positioner, uint32_t token)
{
	(void)client; (void)resource; (void)positioner; (void)token;
}
static const struct xdg_popup_interface sf_xdg_popup_impl = {
	.destroy    = sf_xdg_popup_destroy_impl,
	.grab       = sf_xdg_popup_grab_impl,
	.reposition = sf_xdg_popup_reposition_impl,
};

/* --- xdg_toplevel --- */
static void sf_xdg_toplevel_destroy_impl(struct wl_client *client,
                                         struct wl_resource *resource)
{
	(void)client;
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	if (surface) {
		surface->xdg_toplevel = NULL;
		surface->mapped = 0;
	}
	wl_resource_destroy(resource);
}
static void sf_xdg_toplevel_set_parent_impl(struct wl_client *client,
                                            struct wl_resource *resource,
                                            struct wl_resource *parent)
{
	(void)client; (void)resource; (void)parent;
}
static void sf_xdg_toplevel_set_title_impl(struct wl_client *client,
                                           struct wl_resource *resource,
                                           const char *title)
{
	(void)client;
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	if (surface && title) {
		strncpy(surface->title, title, sizeof(surface->title) - 1);
		surface->title[sizeof(surface->title) - 1] = '\0';
	}
}
static void sf_xdg_toplevel_set_app_id_impl(struct wl_client *client,
                                            struct wl_resource *resource,
                                            const char *app_id)
{
	(void)client; (void)resource; (void)app_id;
}
static void sf_xdg_toplevel_show_window_menu_impl(struct wl_client *client,
                                                  struct wl_resource *resource,
                                                  struct wl_resource *seat,
                                                  uint32_t serial,
                                                  int32_t x, int32_t y)
{
	(void)client; (void)resource; (void)seat; (void)serial; (void)x; (void)y;
}
static void sf_xdg_toplevel_move_impl(struct wl_client *client,
                                      struct wl_resource *resource,
                                      struct wl_resource *seat, uint32_t serial)
{
	(void)client; (void)resource; (void)seat; (void)serial;
}
static void sf_xdg_toplevel_resize_impl(struct wl_client *client,
                                        struct wl_resource *resource,
                                        struct wl_resource *seat, uint32_t serial,
                                        uint32_t edges)
{
	(void)client; (void)resource; (void)seat; (void)serial; (void)edges;
}
static void sf_xdg_toplevel_set_max_size_impl(struct wl_client *client,
                                              struct wl_resource *resource,
                                              int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)width; (void)height;
}
static void sf_xdg_toplevel_set_min_size_impl(struct wl_client *client,
                                              struct wl_resource *resource,
                                              int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)width; (void)height;
}
static void sf_xdg_toplevel_set_maximized_impl(struct wl_client *client,
                                               struct wl_resource *resource)
{
	(void)client;
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	if (surface && surface->comp) {
		surface->x = 0;
		surface->y = 0;
		uint32_t serial = wl_display_next_serial(surface->comp->display);
		struct wl_array states;
		wl_array_init(&states);
		uint32_t *s = wl_array_add(&states, sizeof(uint32_t));
		if (s) *s = XDG_TOPLEVEL_STATE_MAXIMIZED;
		xdg_toplevel_send_configure(resource, surface->comp->output.width,
		                            surface->comp->output.height, &states);
		wl_array_release(&states);
		xdg_surface_send_configure(surface->xdg_surface, serial);
	}
}
static void sf_xdg_toplevel_unset_maximized_impl(struct wl_client *client,
                                                 struct wl_resource *resource)
{
	(void)client;
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	if (surface && surface->comp && surface->xdg_surface) {
		uint32_t serial = wl_display_next_serial(surface->comp->display);
		struct wl_array states;
		wl_array_init(&states);
		xdg_toplevel_send_configure(resource, 0, 0, &states);
		wl_array_release(&states);
		xdg_surface_send_configure(surface->xdg_surface, serial);
	}
}
static void sf_xdg_toplevel_set_fullscreen_impl(struct wl_client *client,
                                                struct wl_resource *resource,
                                                struct wl_resource *output)
{
	(void)client; (void)output;
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	if (surface && surface->comp && surface->xdg_surface) {
		surface->x = 0;
		surface->y = 0;
		uint32_t serial = wl_display_next_serial(surface->comp->display);
		struct wl_array states;
		wl_array_init(&states);
		uint32_t *s = wl_array_add(&states, sizeof(uint32_t));
		if (s) *s = XDG_TOPLEVEL_STATE_FULLSCREEN;
		xdg_toplevel_send_configure(resource, surface->comp->output.width,
		                            surface->comp->output.height, &states);
		wl_array_release(&states);
		xdg_surface_send_configure(surface->xdg_surface, serial);
	}
}
static void sf_xdg_toplevel_unset_fullscreen_impl(struct wl_client *client,
                                                  struct wl_resource *resource)
{
	(void)client;
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	if (surface && surface->comp && surface->xdg_surface) {
		uint32_t serial = wl_display_next_serial(surface->comp->display);
		struct wl_array states;
		wl_array_init(&states);
		xdg_toplevel_send_configure(resource, 0, 0, &states);
		wl_array_release(&states);
		xdg_surface_send_configure(surface->xdg_surface, serial);
	}
}
static void sf_xdg_toplevel_set_minimized_impl(struct wl_client *client,
                                               struct wl_resource *resource)
{
	(void)client;
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	if (surface) {
		surface->mapped = 0;
		surface->buffer_data = NULL;
	}
}

static const struct xdg_toplevel_interface sf_xdg_toplevel_impl = {
	.destroy         = sf_xdg_toplevel_destroy_impl,
	.set_parent      = sf_xdg_toplevel_set_parent_impl,
	.set_title       = sf_xdg_toplevel_set_title_impl,
	.set_app_id      = sf_xdg_toplevel_set_app_id_impl,
	.show_window_menu = sf_xdg_toplevel_show_window_menu_impl,
	.move            = sf_xdg_toplevel_move_impl,
	.resize          = sf_xdg_toplevel_resize_impl,
	.set_max_size    = sf_xdg_toplevel_set_max_size_impl,
	.set_min_size    = sf_xdg_toplevel_set_min_size_impl,
	.set_maximized   = sf_xdg_toplevel_set_maximized_impl,
	.unset_maximized = sf_xdg_toplevel_unset_maximized_impl,
	.set_fullscreen  = sf_xdg_toplevel_set_fullscreen_impl,
	.unset_fullscreen = sf_xdg_toplevel_unset_fullscreen_impl,
	.set_minimized   = sf_xdg_toplevel_set_minimized_impl,
};

/* --- xdg_surface --- */
static void sf_xdg_surface_destroy_impl(struct wl_client *client,
                                        struct wl_resource *resource)
{
	(void)client;
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	if (surface)
		surface->xdg_surface = NULL;
	wl_resource_destroy(resource);
}

static void sf_xdg_surface_get_toplevel_impl(struct wl_client *client,
                                             struct wl_resource *resource,
                                             uint32_t id)
{
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	sf_compositor_t *comp = surface->comp;
	struct wl_resource *r = wl_resource_create(client, &xdg_toplevel_interface,
	                                           SF_XDG_WM_BASE_VERSION, id);
	if (!r) { wl_client_post_no_memory(client); return; }
	wl_resource_set_implementation(r, &sf_xdg_toplevel_impl, surface, NULL);
	surface->xdg_toplevel = r;

	/* 发送初始 configure */
	struct wl_array states;
	wl_array_init(&states);
	xdg_toplevel_send_configure(r, 0, 0, &states);
	wl_array_release(&states);
	uint32_t serial = wl_display_next_serial(comp->display);
	xdg_surface_send_configure(surface->xdg_surface, serial);
}

static void sf_xdg_surface_get_popup_impl(struct wl_client *client,
                                          struct wl_resource *resource,
                                          uint32_t id,
                                          struct wl_resource *parent,
                                          struct wl_resource *positioner)
{
	(void)parent;
	(void)positioner;
	sf_surface_t *surface = wl_resource_get_user_data(resource);
	struct wl_resource *r = wl_resource_create(client, &xdg_popup_interface,
	                                           SF_XDG_WM_BASE_VERSION, id);
	if (!r) { wl_client_post_no_memory(client); return; }
	wl_resource_set_implementation(r, &sf_xdg_popup_impl, surface, NULL);
	/* 简单放置：左上角 */
	xdg_popup_send_configure(r, 0, 0, surface->width, surface->height);
	uint32_t serial = wl_display_next_serial(surface->comp->display);
	xdg_surface_send_configure(surface->xdg_surface, serial);
}

static void sf_xdg_surface_set_window_geometry_impl(struct wl_client *client,
                                                    struct wl_resource *resource,
                                                    int32_t x, int32_t y,
                                                    int32_t width, int32_t height)
{
	(void)client;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	(void)resource;
}

static void sf_xdg_surface_ack_configure_impl(struct wl_client *client,
                                              struct wl_resource *resource,
                                              uint32_t serial)
{
	(void)client; (void)resource; (void)serial;
}

static const struct xdg_surface_interface sf_xdg_surface_impl = {
	.destroy            = sf_xdg_surface_destroy_impl,
	.get_toplevel       = sf_xdg_surface_get_toplevel_impl,
	.get_popup          = sf_xdg_surface_get_popup_impl,
	.set_window_geometry = sf_xdg_surface_set_window_geometry_impl,
	.ack_configure      = sf_xdg_surface_ack_configure_impl,
};

/* --- xdg_wm_base --- */
static void sf_xdg_wm_base_destroy_impl(struct wl_client *client,
                                        struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void sf_xdg_wm_base_create_positioner_impl(struct wl_client *client,
                                                  struct wl_resource *resource,
                                                  uint32_t id)
{
	(void)resource;
	struct wl_resource *r = wl_resource_create(client, &xdg_positioner_interface,
	                                           SF_XDG_WM_BASE_VERSION, id);
	if (!r) { wl_client_post_no_memory(client); return; }
	wl_resource_set_implementation(r, &sf_xdg_positioner_impl, NULL, NULL);
}

static void sf_xdg_wm_base_get_xdg_surface_impl(struct wl_client *client,
                                                struct wl_resource *resource,
                                                uint32_t id,
                                                struct wl_resource *surface_res)
{
	(void)resource;
	sf_surface_t *surface = wl_resource_get_user_data(surface_res);
	if (!surface) {
		wl_client_post_no_memory(client);
		return;
	}
	struct wl_resource *r = wl_resource_create(client, &xdg_surface_interface,
	                                           SF_XDG_WM_BASE_VERSION, id);
	if (!r) { wl_client_post_no_memory(client); return; }
	wl_resource_set_implementation(r, &sf_xdg_surface_impl, surface, NULL);
	surface->xdg_surface = r;
}

static void sf_xdg_wm_base_pong_impl(struct wl_client *client,
                                     struct wl_resource *resource, uint32_t serial)
{
	(void)client; (void)resource; (void)serial;
}

static const struct xdg_wm_base_interface sf_xdg_wm_base_impl = {
	.destroy           = sf_xdg_wm_base_destroy_impl,
	.create_positioner = sf_xdg_wm_base_create_positioner_impl,
	.get_xdg_surface   = sf_xdg_wm_base_get_xdg_surface_impl,
	.pong              = sf_xdg_wm_base_pong_impl,
};

static void sf_xdg_wm_base_bind(struct wl_client *client, void *data,
                                uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *r = wl_resource_create(client, &xdg_wm_base_interface,
	                                           version, id);
	if (!r) { wl_client_post_no_memory(client); return; }
	wl_resource_set_implementation(r, &sf_xdg_wm_base_impl, NULL, NULL);
}

/* ========================================================================
 * Surface 管理
 * ======================================================================== */

sf_surface_t *sf_surface_create(sf_compositor_t *comp, struct wl_client *client,
                                uint32_t id)
{
	sf_surface_t *surface = calloc(1, sizeof(*surface));
	if (!surface) {
		wl_client_post_no_memory(client);
		return NULL;
	}
	surface->comp = comp;
	surface->resource = wl_resource_create(client, &wl_surface_interface,
	                                       SF_COMPOSITOR_VERSION, id);
	if (!surface->resource) {
		free(surface);
		wl_client_post_no_memory(client);
		return NULL;
	}
	wl_resource_set_implementation(surface->resource, &sf_surface_impl, surface,
	                               sf_surface_resource_destroy);
	wl_list_init(&surface->frame_callbacks);
	/* 追加到链表尾部（顶层） */
	wl_list_insert(comp->surfaces.prev, &surface->link);
	return surface;
}

void sf_surface_destroy(sf_surface_t *surface)
{
	if (!surface)
		return;
	if (surface->resource)
		wl_resource_destroy(surface->resource);
	/* sf_surface_resource_destroy 会清理链表与释放内存 */
}

void sf_surface_attach(sf_surface_t *surface, struct wl_resource *buffer,
                       int32_t x, int32_t y)
{
	(void)x;
	(void)y;
	surface->buffer_resource = buffer;
}

/* ========================================================================
 * DRM/KMS 输出
 * ======================================================================== */

static int sf_dumb_fb_create(sf_output_t *output, sf_dumb_fb_t *fb,
                             uint32_t width, uint32_t height)
{
	memset(fb, 0, sizeof(*fb));

	struct drm_mode_create_dumb create;
	memset(&create, 0, sizeof(create));
	create.width = width;
	create.height = height;
	create.bpp = 32;
	if (ioctl(output->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB_NR, &create) < 0) {
		fprintf(stderr, "[sf_compositor] CREATE_DUMB failed\n");
		return -1;
	}
	fb->handle = create.handle;
	fb->pitch  = create.pitch;
	fb->size   = create.size;
	fb->width  = width;
	fb->height = height;

	struct drm_mode_map_dumb map;
	memset(&map, 0, sizeof(map));
	map.handle = create.handle;
	if (ioctl(output->drm_fd, DRM_IOCTL_MODE_MAP_DUMB_NR, &map) < 0) {
		fprintf(stderr, "[sf_compositor] MAP_DUMB failed\n");
		return -1;
	}

	fb->map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
	               output->drm_fd, map.offset);
	if (fb->map == MAP_FAILED) {
		fprintf(stderr, "[sf_compositor] mmap dumb buffer failed\n");
		fb->map = NULL;
		return -1;
	}

	struct drm_mode_fb_cmd addfb;
	memset(&addfb, 0, sizeof(addfb));
	addfb.width = width;
	addfb.height = height;
	addfb.pitch = create.pitch;
	addfb.bpp = 32;
	addfb.depth = 24;
	addfb.handle = create.handle;
	if (ioctl(output->drm_fd, DRM_IOCTL_MODE_ADDFB_NR, &addfb) < 0) {
		fprintf(stderr, "[sf_compositor] ADDFB failed\n");
		munmap(fb->map, fb->size);
		fb->map = NULL;
		return -1;
	}
	fb->fb_id = addfb.fb_id;
	return 0;
}

static void sf_dumb_fb_destroy(sf_output_t *output, sf_dumb_fb_t *fb)
{
	if (fb->fb_id) {
		ioctl(output->drm_fd, DRM_IOCTL_MODE_RMFB_NR, &fb->fb_id);
		fb->fb_id = 0;
	}
	if (fb->map && fb->map != MAP_FAILED) {
		munmap(fb->map, fb->size);
		fb->map = NULL;
	}
	if (fb->handle) {
		struct drm_mode_destroy_dumb d;
		memset(&d, 0, sizeof(d));
		d.handle = fb->handle;
		ioctl(output->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB_NR, &d);
		fb->handle = 0;
	}
}

/* 从 DRM 获取 connector 信息并填充 drmModeConnector 结构 */
static drmModeConnector *sf_drm_get_connector(sf_output_t *output, uint32_t id)
{
	struct drm_mode_get_connector conn;
	memset(&conn, 0, sizeof(conn));
	conn.connector_id = id;
	/* 第一次：获取计数 */
	if (ioctl(output->drm_fd, DRM_IOCTL_MODE_GETCONNECTOR_NR, &conn) < 0)
		return NULL;

	drmModeConnector *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->connector_id   = conn.connector_id;
	c->connector_type = conn.connector_type;
	c->connector_type_id = conn.connector_type_id;
	c->connection     = conn.connection;
	c->mm_width       = conn.mm_width;
	c->mm_height      = conn.mm_height;
	c->subpixel       = conn.subpixel;
	c->encoder_id     = conn.encoder_id;
	c->count_modes    = conn.count_modes;
	c->count_encoders = conn.count_encoders;

	if (c->count_modes > 0) {
		c->modes = calloc(c->count_modes, sizeof(drmModeModeInfo));
		if (!c->modes) {
			free(c);
			return NULL;
		}
	} else {
		c->modes = NULL;
	}
	if (c->count_encoders > 0) {
		c->encoders = calloc(c->count_encoders, sizeof(uint32_t));
		if (!c->encoders) {
			free(c->modes);
			free(c);
			return NULL;
		}
	} else {
		c->encoders = NULL;
	}

	/* 第二次：填充数据 */
	memset(&conn, 0, sizeof(conn));
	conn.connector_id = id;
	conn.modes_ptr    = (uint64_t)(uintptr_t)c->modes;
	conn.encoders_ptr = (uint64_t)(uintptr_t)c->encoders;
	conn.count_modes  = c->count_modes;
	conn.count_encoders = c->count_encoders;
	if (ioctl(output->drm_fd, DRM_IOCTL_MODE_GETCONNECTOR_NR, &conn) < 0) {
		free(c->modes);
		free(c->encoders);
		free(c);
		return NULL;
	}
	return c;
}

int sf_output_init(sf_output_t *output, sf_compositor_t *comp)
{
	memset(output, 0, sizeof(*output));
	output->comp = comp;
	output->drm_fd = -1;
	output->current_fb = 0;
	output->page_flip_pending = 0;

	output->drm_fd = open("/dev/dri/card0", O_RDWR | O_NONBLOCK);
	if (output->drm_fd < 0) {
		fprintf(stderr, "[sf_compositor] 无法打开 /dev/dri/card0: %s\n",
		        strerror(errno));
		return -1;
	}

	/* GETRESOURCES: 先取计数，再取 ID */
	struct drm_mode_card_res res;
	memset(&res, 0, sizeof(res));
	if (ioctl(output->drm_fd, DRM_IOCTL_MODE_GETRESOURCES_NR, &res) < 0) {
		fprintf(stderr, "[sf_compositor] GETRESOURCES 失败\n");
		return -1;
	}
	uint32_t crtcs[32], connectors[32], encoders[32];
	res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtcs;
	res.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
	res.encoder_id_ptr   = (uint64_t)(uintptr_t)encoders;
	if (ioctl(output->drm_fd, DRM_IOCTL_MODE_GETRESOURCES_NR, &res) < 0) {
		fprintf(stderr, "[sf_compositor] GETRESOURCES(2) 失败\n");
		return -1;
	}

	/* 查找已连接的 connector */
	drmModeConnector *conn = NULL;
	for (uint32_t i = 0; i < res.count_connectors; i++) {
		drmModeConnector *c = sf_drm_get_connector(output, connectors[i]);
		if (!c)
			continue;
		if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
			conn = c;
			break;
		}
		free(c->modes);
		free(c->encoders);
		free(c);
	}
	if (!conn) {
		fprintf(stderr, "[sf_compositor] 未找到已连接的显示器\n");
		return -1;
	}
	output->connector = conn;
	output->connector_id = conn->connector_id;
	output->encoder_id = conn->encoder_id;
	output->mm_width = conn->mm_width;
	output->mm_height = conn->mm_height;

	/* 选择首选模式 (找不到则用第一个) */
	drmModeModeInfo *mode = &conn->modes[0];
	for (uint32_t i = 0; i < conn->count_modes; i++) {
		if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			mode = &conn->modes[i];
			break;
		}
	}
	output->mode = *mode;
	output->width = mode->hdisplay;
	output->height = mode->vdisplay;
	output->refresh_rate = mode->vrefresh;

	/* GETENCODER 获取 CRTC */
	uint32_t crtc_id = 0;
	if (conn->encoder_id) {
		struct drm_mode_get_encoder enc;
		memset(&enc, 0, sizeof(enc));
		enc.encoder_id = conn->encoder_id;
		if (ioctl(output->drm_fd, DRM_IOCTL_MODE_GETENCODER_NR, &enc) == 0)
			crtc_id = enc.crtc_id;
	}
	if (crtc_id == 0 && res.count_crtcs > 0)
		crtc_id = crtcs[0];
	if (crtc_id == 0) {
		fprintf(stderr, "[sf_compositor] 未找到可用 CRTC\n");
		return -1;
	}
	output->crtc_id = crtc_id;

	/* 创建两个 dumb 帧缓冲 (双缓冲) */
	for (int i = 0; i < 2; i++) {
		if (sf_dumb_fb_create(output, &output->fb[i],
		                      output->width, output->height) < 0) {
			fprintf(stderr, "[sf_compositor] 创建 dumb fb[%d] 失败\n", i);
			return -1;
		}
	}

	/* SETCRTC 启用显示 */
	struct drm_mode_crtc c;
	memset(&c, 0, sizeof(c));
	c.crtc_id = output->crtc_id;
	c.fb_id = output->fb[0].fb_id;
	c.set_connectors_ptr = (uint64_t)(uintptr_t)&output->connector_id;
	c.count_connectors = 1;
	c.mode_valid = 1;
	c.mode = output->mode;
	if (ioctl(output->drm_fd, DRM_IOCTL_MODE_SETCRTC_NR, &c) < 0) {
		const char *_e = "[SF] SETCRTC FAILED\n";
		write(2, _e, strlen(_e));
		return -1;
	}
	output->current_fb = 0;
	{
		const char *_m = "[SF] SETCRTC OK\n";
		write(2, _m, strlen(_m));
	}

	/* 注册 DRM fd 到事件循环 (page flip 事件) */
	{
		const char *_m2 = "[SF] before wl_event_loop_add_fd\n";
		write(2, _m2, strlen(_m2));
	}
	comp->drm_event_source = wl_event_loop_add_fd(comp->event_loop,
	                                              output->drm_fd,
	                                              WL_EVENT_READABLE,
	                                              sf_drm_event_handler, comp);
	{
		const char *_m3 = "[SF] after wl_event_loop_add_fd\n";
		write(2, _m3, strlen(_m3));
	}

	fprintf(stderr, "[sf_compositor] 输出初始化完成: %dx%d@%d mHz, crtc=%u\n",
	        output->width, output->height, output->refresh_rate, output->crtc_id);
	return 0;
}

static int sf_drm_event_handler(int fd, uint32_t mask, void *data)
{
	(void)mask;
	sf_compositor_t *comp = data;
	/* 读取并丢弃所有待处理 DRM 事件 (page flip 完成) */
	char buf[256];
	while (read(fd, buf, sizeof(buf)) > 0) {
		/* 排空 */
	}
	comp->output.page_flip_pending = 0;
	return 0;
}

void sf_output_render(sf_output_t *output, sf_compositor_t *comp)
{
	(void)comp;
	int back = 1 - output->current_fb;
	struct drm_mode_crtc_page_flip pf;
	memset(&pf, 0, sizeof(pf));
	pf.fb_id = output->fb[back].fb_id;
	pf.crtc_id = output->crtc_id;
	pf.flags = 0;  /* 立即翻转，不请求事件 */
	pf.user_data = 0;
	if (ioctl(output->drm_fd, DRM_IOCTL_MODE_PAGE_FLIP_NR, &pf) < 0) {
		/* 翻转失败时回退到 SETCRTC */
		struct drm_mode_crtc c;
		memset(&c, 0, sizeof(c));
		c.crtc_id = output->crtc_id;
		c.fb_id = output->fb[back].fb_id;
		c.set_connectors_ptr = (uint64_t)(uintptr_t)&output->connector_id;
		c.count_connectors = 1;
		c.mode_valid = 1;
		c.mode = output->mode;
		ioctl(output->drm_fd, DRM_IOCTL_MODE_SETCRTC_NR, &c);
	}
	output->current_fb = back;
}

void sf_output_finish(sf_output_t *output)
{
	for (int i = 0; i < 2; i++)
		sf_dumb_fb_destroy(output, &output->fb[i]);
	if (output->connector) {
		drmModeConnector *conn = (drmModeConnector *)output->connector;
		free(conn->modes);
		free(conn->encoders);
		free(conn);
		output->connector = NULL;
	}
	if (output->drm_fd >= 0) {
		close(output->drm_fd);
		output->drm_fd = -1;
	}
}

/* ========================================================================
 * evdev 输入
 * ======================================================================== */

static int sf_keyboard_fd_handler(int fd, uint32_t mask, void *data);
static int sf_pointer_fd_handler(int fd, uint32_t mask, void *data);

int sf_seat_init(sf_seat_t *seat, sf_compositor_t *comp)
{
	memset(seat, 0, sizeof(*seat));
	seat->comp = comp;
	seat->keyboard_fd = -1;
	seat->pointer_fd = -1;
	seat->pointer_x = comp->output.width / 2;
	seat->pointer_y = comp->output.height / 2;

	/* 键盘: /dev/input/event0 */
	seat->keyboard_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	if (seat->keyboard_fd >= 0) {
		comp->keyboard_event_source = wl_event_loop_add_fd(
			comp->event_loop, seat->keyboard_fd,
			WL_EVENT_READABLE, sf_keyboard_fd_handler, comp);
		fprintf(stderr, "[sf_compositor] 键盘 event0 已打开\n");
	} else {
		fprintf(stderr, "[sf_compositor] 无法打开 /dev/input/event0\n");
	}

	/* 鼠标: /dev/input/event1 (若可用) */
	seat->pointer_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
	if (seat->pointer_fd >= 0) {
		comp->pointer_event_source = wl_event_loop_add_fd(
			comp->event_loop, seat->pointer_fd,
			WL_EVENT_READABLE, sf_pointer_fd_handler, comp);
		fprintf(stderr, "[sf_compositor] 鼠标 event1 已打开\n");
	} else {
		fprintf(stderr, "[sf_compositor] 无法打开 /dev/input/event1\n");
	}

	return 0;
}

static int sf_keyboard_fd_handler(int fd, uint32_t mask, void *data)
{
	(void)fd;
	(void)mask;
	sf_compositor_t *comp = data;
	sf_seat_handle_keyboard_event(&comp->seat, comp);
	return 0;
}

static int sf_pointer_fd_handler(int fd, uint32_t mask, void *data)
{
	(void)fd;
	(void)mask;
	sf_compositor_t *comp = data;
	sf_seat_handle_pointer_event(&comp->seat, comp);
	return 0;
}

void sf_seat_handle_keyboard_event(sf_seat_t *seat, sf_compositor_t *comp)
{
	struct input_event ev;
	while (read(seat->keyboard_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (ev.type != EV_KEY)
			continue;

		uint32_t mod_mask = 0;
		int is_modifier = 0;
		switch (ev.code) {
		case KEY_LEFTSHIFT:
		case KEY_RIGHTSHIFT:
			mod_mask = SF_MOD_SHIFT;
			is_modifier = 1;
			break;
		case KEY_LEFTCTRL:
		case KEY_RIGHTCTRL:
			mod_mask = SF_MOD_CTRL;
			is_modifier = 1;
			break;
		case KEY_LEFTALT:
		case KEY_RIGHTALT:
			mod_mask = SF_MOD_ALT;
			is_modifier = 1;
			break;
		case KEY_LEFTMETA:
		case KEY_RIGHTMETA:
			mod_mask = SF_MOD_LOGO;
			is_modifier = 1;
			break;
		case KEY_CAPSLOCK:
			if (ev.value)
				seat->keyboard_modifiers ^= SF_MOD_CAPS;
			is_modifier = 1;
			break;
		default:
			break;
		}

		if (mod_mask) {
			if (ev.value)
				seat->keyboard_modifiers |= mod_mask;
			else
				seat->keyboard_modifiers &= ~mod_mask;
		}

		if (is_modifier && seat->keyboard_resource) {
			uint32_t serial = wl_display_next_serial(comp->display);
			uint32_t depressed = seat->keyboard_modifiers & ~SF_MOD_CAPS;
			uint32_t locked = seat->keyboard_modifiers & SF_MOD_CAPS;
			wl_keyboard_send_modifiers(seat->keyboard_resource, serial,
			                           depressed, 0, locked, 0);
		}

		if (seat->keyboard_resource && seat->keyboard_focus &&
		    seat->keyboard_focus->resource) {
			uint32_t serial = wl_display_next_serial(comp->display);
			uint32_t time = sf_time_ms();
			wl_keyboard_send_key(seat->keyboard_resource, serial, time,
			                     ev.code,
			                     ev.value ? WL_KEYBOARD_KEY_STATE_PRESSED
			                              : WL_KEYBOARD_KEY_STATE_RELEASED);
		}
	}
	sf_compositor_composite(comp);
}

void sf_seat_handle_pointer_event(sf_seat_t *seat, sf_compositor_t *comp)
{
	struct input_event ev;
	int had_motion = 0;
	int had_button = 0;

	while (read(seat->pointer_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (ev.type == EV_REL) {
			switch (ev.code) {
			case REL_X:
				seat->pointer_x += ev.value;
				had_motion = 1;
				break;
			case REL_Y:
				seat->pointer_y += ev.value;
				had_motion = 1;
				break;
			case REL_WHEEL:
				if (seat->pointer_resource && seat->pointer_focus) {
					wl_pointer_send_axis(seat->pointer_resource,
					                     sf_time_ms(),
					                     WL_POINTER_AXIS_VERTICAL_SCROLL,
					                     wl_fixed_from_int(ev.value));
				}
				break;
			default:
				break;
			}
		} else if (ev.type == EV_KEY) {
			uint32_t btn = 0;
			if (ev.code == BTN_LEFT)        btn = BTN_LEFT;
			else if (ev.code == BTN_RIGHT)  btn = BTN_RIGHT;
			else if (ev.code == BTN_MIDDLE) btn = BTN_MIDDLE;
			if (btn && seat->pointer_resource && seat->pointer_focus) {
				uint32_t serial = wl_display_next_serial(comp->display);
				wl_pointer_send_button(seat->pointer_resource, serial,
				                       sf_time_ms(), btn,
				                       ev.value
				                           ? WL_POINTER_BUTTON_STATE_PRESSED
				                           : WL_POINTER_BUTTON_STATE_RELEASED);
				had_button = 1;
				/* 点击时将键盘焦点切换到该表面 */
				if (ev.value)
					sf_seat_set_keyboard_focus(seat, comp,
					                           seat->pointer_focus);
			}
		}
	}

	/* 限制指针在屏幕范围内 */
	if (had_motion) {
		if (seat->pointer_x < 0) seat->pointer_x = 0;
		if (seat->pointer_y < 0) seat->pointer_y = 0;
		if (seat->pointer_x >= comp->output.width)
			seat->pointer_x = comp->output.width - 1;
		if (seat->pointer_y >= comp->output.height)
			seat->pointer_y = comp->output.height - 1;

		sf_surface_t *focus = sf_surface_at(comp, seat->pointer_x,
		                                    seat->pointer_y);
		if (focus != seat->pointer_focus)
			sf_seat_set_pointer_focus(seat, comp, focus);

		if (seat->pointer_resource && seat->pointer_focus) {
			wl_pointer_send_motion(seat->pointer_resource, sf_time_ms(),
				wl_fixed_from_int(seat->pointer_x - seat->pointer_focus->x),
				wl_fixed_from_int(seat->pointer_y - seat->pointer_focus->y));
		}
	}
	(void)had_button;
	sf_compositor_composite(comp);
}

void sf_seat_finish(sf_seat_t *seat)
{
	if (seat->keyboard_fd >= 0) {
		close(seat->keyboard_fd);
		seat->keyboard_fd = -1;
	}
	if (seat->pointer_fd >= 0) {
		close(seat->pointer_fd);
		seat->pointer_fd = -1;
	}
}

/* ========================================================================
 * 软件合成
 * ======================================================================== */

static void sf_blend_surface(sf_dumb_fb_t *fb, uint32_t fb_w, uint32_t fb_h,
                             sf_surface_t *s)
{
	int32_t dx0 = s->x;
	int32_t dy0 = s->y;
	int32_t dx1 = s->x + s->width;
	int32_t dy1 = s->y + s->height;
	if (dx0 < 0) dx0 = 0;
	if (dy0 < 0) dy0 = 0;
	if (dx1 > (int32_t)fb_w) dx1 = fb_w;
	if (dy1 > (int32_t)fb_h) dy1 = fb_h;
	if (dx0 >= dx1 || dy0 >= dy1)
		return;

	int has_alpha = (s->buffer_format == WL_SHM_FORMAT_ARGB8888);
	uint32_t src_stride = s->buffer_stride;

	for (int32_t y = dy0; y < dy1; y++) {
		uint32_t *dst = (uint32_t *)((char *)fb->map + y * fb->pitch + dx0 * 4);
		uint32_t *src = (uint32_t *)((char *)s->buffer_data +
		                             (y - s->y) * src_stride +
		                             (dx0 - s->x) * 4);
		for (int32_t x = dx0; x < dx1; x++) {
			uint32_t pixel = *src++;
			if (has_alpha) {
				uint8_t a = (uint8_t)((pixel >> 24) & 0xff);
				if (a == 0) {
					dst++;
					continue;
				}
				if (a == 255) {
					*dst++ = pixel | 0xff000000u;
					continue;
				}
				uint32_t d = *dst;
				uint8_t sr = (uint8_t)((pixel >> 16) & 0xff);
				uint8_t sg = (uint8_t)((pixel >> 8) & 0xff);
				uint8_t sb = (uint8_t)(pixel & 0xff);
				uint8_t dr = (uint8_t)((d >> 16) & 0xff);
				uint8_t dg = (uint8_t)((d >> 8) & 0xff);
				uint8_t db = (uint8_t)(d & 0xff);
				uint8_t r = (uint8_t)((sr * a + dr * (255 - a)) / 255);
				uint8_t g = (uint8_t)((sg * a + dg * (255 - a)) / 255);
				uint8_t b = (uint8_t)((sb * a + db * (255 - a)) / 255);
				*dst++ = 0xff000000u | (r << 16) | (g << 8) | b;
			} else {
				*dst++ = pixel | 0xff000000u;
			}
		}
	}
}

void sf_compositor_composite(sf_compositor_t *comp)
{
	sf_output_t *output = &comp->output;
	if (!output->fb[0].map || !output->fb[1].map)
		return;

	int back = 1 - output->current_fb;
	sf_dumb_fb_t *fb = &output->fb[back];

	/* 清屏为黑色 */
	memset(fb->map, 0, fb->size);

	/* 按 z 序 (从后到前) 合成所有已映射表面 */
	sf_surface_t *surface;
	wl_list_for_each(surface, &comp->surfaces, link) {
		if (!surface->mapped || !surface->buffer_data)
			continue;
		sf_blend_surface(fb, output->width, output->height, surface);
	}

	/* 翻转显示 */
	sf_output_render(output, comp);

	/* 发送帧回调 */
	uint32_t time = sf_time_ms();
	sf_surface_t *s;
	wl_list_for_each(s, &comp->surfaces, link) {
		sf_frame_callback_t *cb, *tmp;
		wl_list_for_each_safe(cb, tmp, &s->frame_callbacks, link) {
			wl_callback_send_done(cb->resource, time);
			wl_resource_destroy(cb->resource);
			wl_list_remove(&cb->link);
			free(cb);
		}
	}
}

/* ========================================================================
 * 合成器主循环
 * ======================================================================== */

int sf_compositor_init(sf_compositor_t *comp)
{
	memset(comp, 0, sizeof(*comp));
	comp->running = 0;

	comp->display = wl_display_create();
	if (!comp->display) {
		fprintf(stderr, "[sf_compositor] wl_display_create 失败\n");
		return -1;
	}
	comp->event_loop = wl_display_get_event_loop(comp->display);
	wl_list_init(&comp->surfaces);

	/* 初始化输出 (DRM/KMS) */
	fprintf(stderr, "[sf_compositor] initializing output...\n");
	if (sf_output_init(&comp->output, comp) < 0) {
		fprintf(stderr, "[sf_compositor] 输出初始化失败\n");
		return -1;
	}
	fprintf(stderr, "[sf_compositor] output init done\n");

	/* 初始化输入 (evdev) */
	fprintf(stderr, "[sf_compositor] initializing seat...\n");
	if (sf_seat_init(&comp->seat, comp) < 0) {
		fprintf(stderr, "[sf_compositor] 输入初始化失败\n");
		return -1;
	}
	fprintf(stderr, "[sf_compositor] seat init done\n");

	/* 注册 Wayland 全局对象 */
	fprintf(stderr, "[sf_compositor] registering globals...\n");
	comp->compositor_global = wl_global_create(comp->display,
		&wl_compositor_interface, SF_COMPOSITOR_VERSION, comp,
		sf_compositor_bind);
	fprintf(stderr, "[sf_compositor] compositor_global=%p\n", (void *)comp->compositor_global);
	comp->shm_global = wl_global_create(comp->display, &wl_shm_interface,
		SF_SHM_VERSION, comp, sf_shm_bind);
	fprintf(stderr, "[sf_compositor] shm_global=%p\n", (void *)comp->shm_global);
	comp->output_global = wl_global_create(comp->display,
		&wl_output_interface, SF_OUTPUT_VERSION, &comp->output,
		sf_output_bind);
	fprintf(stderr, "[sf_compositor] output_global=%p\n", (void *)comp->output_global);
	comp->seat_global = wl_global_create(comp->display, &wl_seat_interface,
		SF_SEAT_VERSION, comp, sf_seat_bind);
	fprintf(stderr, "[sf_compositor] seat_global=%p\n", (void *)comp->seat_global);
	comp->xdg_wm_base_global = wl_global_create(comp->display,
		&xdg_wm_base_interface, SF_XDG_WM_BASE_VERSION, comp,
		sf_xdg_wm_base_bind);
	fprintf(stderr, "[sf_compositor] xdg_wm_base_global=%p\n", (void *)comp->xdg_wm_base_global);

	/* 添加 Wayland socket */
	fprintf(stderr, "[sf_compositor] adding socket...\n");
	if (wl_display_add_socket(comp->display, "wayland-0") < 0) {
		fprintf(stderr, "[sf_compositor] 无法添加 socket wayland-0\n");
		return -1;
	}

	fprintf(stderr, "[sf_compositor] 合成器已就绪, socket=wayland-0\n");
	return 0;
}

void sf_compositor_run(sf_compositor_t *comp)
{
	comp->running = 1;

	/* 初始渲染：绘制欢迎画面（蓝色渐变背景 + 白色标题文字） */
	{
		sf_output_t *output = &comp->output;
		if (output->fb[0].map && output->fb[1].map) {
			int back = 1 - output->current_fb;
			sf_dumb_fb_t *fb = &output->fb[back];
			uint32_t *pixels = (uint32_t *)fb->map;
			int stride = fb->pitch / 4;

			/* 绘制蓝色渐变背景 */
			for (int y = 0; y < output->height; y++) {
				uint8_t r = (uint8_t)(0x0C + (y * 0x20 / output->height));
				uint8_t g = (uint8_t)(0x16 + (y * 0x30 / output->height));
				uint8_t b = (uint8_t)(0x50 + (y * 0x60 / output->height));
				uint32_t color = 0xFF000000u | (r << 16) | (g << 8) | b;
				for (int x = 0; x < output->width; x++) {
					pixels[y * stride + x] = color;
				}
			}

			/* 中央绘制白色方块作为 SpiritFoxOS 标识 */
			int box_w = 400, box_h = 120;
			int box_x = (output->width - box_w) / 2;
			int box_y = (output->height - box_h) / 2;
			for (int y = box_y; y < box_y + box_h; y++) {
				for (int x = box_x; x < box_x + box_w; x++) {
					/* 半透明白色方框边框 */
					if (y == box_y || y == box_y + box_h - 1 ||
					    x == box_x || x == box_x + box_w - 1) {
						pixels[y * stride + x] = 0xFFFFFFFF;
					} else {
						/* 内部深蓝色 */
						pixels[y * stride + x] = 0xFF102040;
					}
				}
			}

			sf_output_render(output, comp);
		}
		fprintf(stderr, "[sf_compositor] 欢迎画面已渲染\n");
	}

	/* wl_display_run 处理事件循环: 客户端连接、DRM 事件、evdev 事件 */
	wl_display_run(comp->display);
}

void sf_compositor_finish(sf_compositor_t *comp)
{
	if (!comp)
		return;

	/* 移除事件源 */
	if (comp->drm_event_source) {
		wl_event_source_remove(comp->drm_event_source);
		comp->drm_event_source = NULL;
	}
	if (comp->keyboard_event_source) {
		wl_event_source_remove(comp->keyboard_event_source);
		comp->keyboard_event_source = NULL;
	}
	if (comp->pointer_event_source) {
		wl_event_source_remove(comp->pointer_event_source);
		comp->pointer_event_source = NULL;
	}

	/* 销毁全局对象 */
	if (comp->compositor_global)    wl_global_destroy(comp->compositor_global);
	if (comp->shm_global)           wl_global_destroy(comp->shm_global);
	if (comp->output_global)        wl_global_destroy(comp->output_global);
	if (comp->seat_global)          wl_global_destroy(comp->seat_global);
	if (comp->xdg_wm_base_global)   wl_global_destroy(comp->xdg_wm_base_global);

	/* 销毁所有 surface */
	sf_surface_t *surface, *tmp;
	wl_list_for_each_safe(surface, tmp, &comp->surfaces, link) {
		if (surface->resource)
			wl_resource_destroy(surface->resource);
	}

	sf_seat_finish(&comp->seat);
	sf_output_finish(&comp->output);

	if (comp->display) {
		wl_display_destroy_clients(comp->display);
		wl_display_destroy(comp->display);
		comp->display = NULL;
	}
}
