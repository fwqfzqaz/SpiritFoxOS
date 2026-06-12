/* SpiritFoxOS - 帧缓冲区接口
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
#ifndef FB_H
#define FB_H

#include <stdint.h>

typedef struct {
    uint32_t *buffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;       /* 每行字节数 */
    uint32_t bpp;
    uint64_t phys_addr;
} framebuffer_t;

extern framebuffer_t fb;

void fb_init(uint64_t addr, uint32_t width, uint32_t height,
             uint32_t pitch, uint32_t bpp);
void fb_draw_pixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t fb_get_pixel(uint32_t x, uint32_t y);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color);
void fb_clear(uint32_t color);
void fb_swap_buffers(void);  /* 双缓冲 */
uint32_t *fb_get_draw_buffer(void);  /* 返回后缓冲区或前缓冲区 */

/* ---- 快速路径原语（无边界检查，供内部使用） ---- */
void fb_fill_rect_fast(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_hline_fast(uint32_t x, uint32_t y, uint32_t w, uint32_t color);
void fb_memset32(uint32_t *dst, uint32_t val, uint64_t count);
void fb_memcpy32(uint32_t *dst, const uint32_t *src, uint64_t count);

/* 颜色宏 - 0xRRGGBB格式 */
#define FB_RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define FB_RGBA(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* 常用颜色 */
#define FB_BLACK     FB_RGB(0, 0, 0)
#define FB_WHITE     FB_RGB(255, 255, 255)
#define FB_RED       FB_RGB(220, 50, 50)
#define FB_GREEN     FB_RGB(50, 205, 50)
#define FB_BLUE      FB_RGB(50, 100, 220)
#define FB_CYAN      FB_RGB(0, 210, 210)
#define FB_YELLOW    FB_RGB(255, 220, 50)
#define FB_MAGENTA   FB_RGB(200, 50, 200)
#define FB_ORANGE    FB_RGB(255, 150, 30)
#define FB_GREY      FB_RGB(128, 128, 128)
#define FB_DARK_GREY FB_RGB(64, 64, 64)
#define FB_LIGHT_GREY FB_RGB(192, 192, 192)

/* SpiritFoxOS 主题颜色 */
#define FB_SF_ORANGE  FB_RGB(255, 140, 50)
#define FB_SF_DARK    FB_RGB(30, 30, 40)
#define FB_SF_PANEL   FB_RGB(45, 45, 60)
#define FB_SF_ACCENT  FB_RGB(80, 160, 255)

#endif /* FB_H */
