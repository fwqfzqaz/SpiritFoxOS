/* SpiritFoxOS - 帧缓冲区管理
 * Copyright (C) 2025 SpiritFoxOS Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "fb.h"
#include "vmm.h"
#include "pmm.h"
#include "../include/string.h"

framebuffer_t fb;
static uint32_t *back_buffer = NULL;

void fb_init(uint64_t addr, uint32_t width, uint32_t height,
             uint32_t pitch, uint32_t bpp) {
    fb.phys_addr = addr;
    fb.width = width;
    fb.height = height;
    fb.pitch = pitch;
    fb.bpp = bpp;

    /* 在4GB恒等映射下，帧缓冲区物理地址
     * （通常为0xE0000000）可作为虚拟地址直接访问 */
    fb.buffer = (uint32_t *)(uint64_t)addr;

    /* 为双缓冲分配后缓冲区 */
    uint64_t fb_size = (uint64_t)height * pitch;
    uint64_t bb_pages = (fb_size + 4095) / 4096;

    /* 尝试为后缓冲区分配连续的物理内存 */
    uint64_t bb_phys = pmm_alloc_pages(bb_pages);
    if (bb_phys && bb_phys < 0x100000000ULL) {
        /* 后缓冲区在恒等映射的4GB范围内 */
        back_buffer = (uint32_t *)bb_phys;
        /* 使用快速填充清空后缓冲区 */
        fb_memset32(back_buffer, 0, fb_size / 4);
    } else {
        back_buffer = NULL;
        /* 如果分配失败，将直接绘制到前缓冲区 */
    }
}

/* ---- 快速32位memset：用指定值填充count个uint32_t字 ---- */
void fb_memset32(uint32_t *dst, uint32_t val, uint64_t count) {
    for (uint64_t i = 0; i < count; i++)
        dst[i] = val;
}

/* ---- 快速块拷贝：从src复制count个uint32_t字到dst ---- */
void fb_memcpy32(uint32_t *dst, const uint32_t *src, uint64_t count) {
    for (uint64_t i = 0; i < count; i++)
        dst[i] = src[i];
}

/* ---- 像素级操作（带边界检查，保留兼容性） ---- */

void fb_draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb.width || y >= fb.height) return;
    uint32_t *target = back_buffer ? back_buffer : fb.buffer;
    uint32_t *pixel = (uint32_t *)((uint8_t *)target + y * fb.pitch + x * (fb.bpp / 8));
    *pixel = color;
}

uint32_t fb_get_pixel(uint32_t x, uint32_t y) {
    if (x >= fb.width || y >= fb.height) return 0;
    uint32_t *target = back_buffer ? back_buffer : fb.buffer;
    uint32_t *pixel = (uint32_t *)((uint8_t *)target + y * fb.pitch + x * (fb.bpp / 8));
    return *pixel;
}

/* ---- 优化的填充矩形：使用字大小存储代替逐像素循环 ---- */
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    /* 限制在屏幕边界内 */
    if (x >= fb.width || y >= fb.height) return;
    if (x + w > fb.width) w = fb.width - x;
    if (y + h > fb.height) h = fb.height - y;
    if (w == 0 || h == 0) return;

    uint32_t *target = back_buffer ? back_buffer : fb.buffer;
    uint32_t stride = fb.pitch / 4;  /* 以uint32_t为单位的步长 */

    /* 使用顺序32位写入填充每一行。
     * GCC -O2将展开此循环并使用rep stos或类似指令。 */
    for (uint32_t dy = 0; dy < h; dy++) {
        uint32_t *row = target + ((y + dy) * stride) + x;
        /* 展开4次一组的填充以获得最大吞吐量 */
        uint32_t dx = 0;
        uint32_t w4 = w & ~3u;
        for (; dx < w4; dx += 4) {
            row[dx]     = color;
            row[dx + 1] = color;
            row[dx + 2] = color;
            row[dx + 3] = color;
        }
        /* 剩余部分 */
        for (; dx < w; dx++)
            row[dx] = color;
    }
}

/* ---- 快速填充矩形：无边界检查。调用者保证坐标有效。
 * 用于坐标已预先裁剪的内部渲染。 ---- */
void fb_fill_rect_fast(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    uint32_t *target = back_buffer ? back_buffer : fb.buffer;
    uint32_t stride = fb.pitch / 4;

    for (uint32_t dy = 0; dy < h; dy++) {
        uint32_t *row = target + ((y + dy) * stride) + x;
        uint32_t dx = 0;
        uint32_t w4 = w & ~3u;
        for (; dx < w4; dx += 4) {
            row[dx]     = color;
            row[dx + 1] = color;
            row[dx + 2] = color;
            row[dx + 3] = color;
        }
        for (; dx < w; dx++)
            row[dx] = color;
    }
}

/* ---- 快速水平线填充（单行，无边界检查） ---- */
void fb_hline_fast(uint32_t x, uint32_t y, uint32_t w, uint32_t color) {
    uint32_t *target = back_buffer ? back_buffer : fb.buffer;
    uint32_t stride = fb.pitch / 4;
    uint32_t *row = target + (y * stride) + x;

    uint32_t dx = 0;
    uint32_t w4 = w & ~3u;
    for (; dx < w4; dx += 4) {
        row[dx]     = color;
        row[dx + 1] = color;
        row[dx + 2] = color;
        row[dx + 3] = color;
    }
    for (; dx < w; dx++)
        row[dx] = color;
}

void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    fb_fill_rect(x, y, w, 1, color);
    fb_fill_rect(x, y + h - 1, w, 1, color);
    fb_fill_rect(x, y, 1, h, color);
    fb_fill_rect(x + w - 1, y, 1, h, color);
}

void fb_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color) {
    int dx = (int)x1 - (int)x0;
    int dy = (int)y1 - (int)y0;
    int sx = dx > 0 ? 1 : -1;
    int sy = dy > 0 ? 1 : -1;
    dx = dx > 0 ? dx : -dx;
    dy = dy > 0 ? dy : -dy;
    int err = (dx > dy ? dx : -dy) / 2;

    while (1) {
        fb_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 < dy) { err += dx; y0 += sy; }
    }
}

void fb_clear(uint32_t color) {
    fb_fill_rect(0, 0, fb.width, fb.height, color);
}

void fb_swap_buffers(void) {
    if (!back_buffer) return;
    uint64_t total_pixels = (uint64_t)fb.height * (fb.pitch / 4);
    fb_memcpy32(fb.buffer, back_buffer, total_pixels);
}

uint32_t *fb_get_draw_buffer(void) {
    return back_buffer ? back_buffer : fb.buffer;
}
