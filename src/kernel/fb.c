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

    /* With 4GB identity mapping, the framebuffer physical address
     * (typically 0xE0000000) is directly accessible as a virtual address */
    fb.buffer = (uint32_t *)(uint64_t)addr;

    /* Allocate back buffer for double buffering */
    uint64_t fb_size = (uint64_t)height * pitch;
    uint64_t bb_pages = (fb_size + 4095) / 4096;

    /* Try to allocate contiguous physical memory for back buffer */
    uint64_t bb_phys = pmm_alloc_pages(bb_pages);
    if (bb_phys && bb_phys < 0x100000000ULL) {
        /* Back buffer is within identity-mapped 4GB range */
        back_buffer = (uint32_t *)bb_phys;
        /* Clear back buffer using fast fill */
        fb_memset32(back_buffer, 0, fb_size / 4);
    } else {
        back_buffer = NULL;
        /* If allocation failed, we'll draw directly to the front buffer */
    }
}

/* ---- Fast 32-bit memset: fill count uint32_t words with value ---- */
void fb_memset32(uint32_t *dst, uint32_t val, uint64_t count) {
    for (uint64_t i = 0; i < count; i++)
        dst[i] = val;
}

/* ---- Fast block copy: copy count uint32_t words from src to dst ---- */
void fb_memcpy32(uint32_t *dst, const uint32_t *src, uint64_t count) {
    for (uint64_t i = 0; i < count; i++)
        dst[i] = src[i];
}

/* ---- Pixel-level operations (with bounds checking, kept for compatibility) ---- */

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

/* ---- Optimized fill rect: uses word-size stores instead of per-pixel loop ---- */
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    /* Clamp to screen bounds */
    if (x >= fb.width || y >= fb.height) return;
    if (x + w > fb.width) w = fb.width - x;
    if (y + h > fb.height) h = fb.height - y;
    if (w == 0 || h == 0) return;

    uint32_t *target = back_buffer ? back_buffer : fb.buffer;
    uint32_t stride = fb.pitch / 4;  /* stride in uint32_t units */

    /* Fill each row using sequential 32-bit writes.
     * GCC -O2 will unroll this loop and use rep stos or similar. */
    for (uint32_t dy = 0; dy < h; dy++) {
        uint32_t *row = target + ((y + dy) * stride) + x;
        /* Unrolled 4-at-a-time fill for maximum throughput */
        uint32_t dx = 0;
        uint32_t w4 = w & ~3u;
        for (; dx < w4; dx += 4) {
            row[dx]     = color;
            row[dx + 1] = color;
            row[dx + 2] = color;
            row[dx + 3] = color;
        }
        /* Remainder */
        for (; dx < w; dx++)
            row[dx] = color;
    }
}

/* ---- Fast fill rect: NO bounds checking. Caller guarantees valid coords.
 * Used by internal rendering where coordinates are pre-clamped. ---- */
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

/* ---- Fast horizontal line fill (single row, no bounds check) ---- */
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
