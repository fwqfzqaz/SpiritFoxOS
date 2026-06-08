#ifndef FB_H
#define FB_H

#include <stdint.h>

typedef struct {
    uint32_t *buffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;       /* bytes per row */
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
void fb_swap_buffers(void);  /* Double buffering */
uint32_t *fb_get_draw_buffer(void);  /* Returns back buffer or front buffer */

/* Color macros - 0xRRGGBB format */
#define FB_RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define FB_RGBA(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* Common colors */
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

/* SpiritFoxOS theme colors */
#define FB_SF_ORANGE  FB_RGB(255, 140, 50)
#define FB_SF_DARK    FB_RGB(30, 30, 40)
#define FB_SF_PANEL   FB_RGB(45, 45, 60)
#define FB_SF_ACCENT  FB_RGB(80, 160, 255)

#endif /* FB_H */
