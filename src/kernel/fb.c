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
        /* Clear back buffer */
        for (uint64_t i = 0; i < fb_size / 4; i++) {
            back_buffer[i] = 0;
        }
    } else {
        back_buffer = NULL;
        /* If allocation failed, we'll draw directly to the front buffer */
    }
}

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

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (x + w > fb.width) w = fb.width - x;
    if (y + h > fb.height) h = fb.height - y;

    uint32_t *target = back_buffer ? back_buffer : fb.buffer;
    uint32_t bytes_per_pixel = fb.bpp / 8;

    for (uint32_t dy = 0; dy < h; dy++) {
        uint32_t *row = (uint32_t *)((uint8_t *)target + (y + dy) * fb.pitch + x * bytes_per_pixel);
        for (uint32_t dx = 0; dx < w; dx++) {
            row[dx] = color;
        }
    }
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
    uint64_t total_bytes = (uint64_t)fb.height * fb.pitch;
    memcpy(fb.buffer, back_buffer, (size_t)total_bytes);
}

uint32_t *fb_get_draw_buffer(void) {
    return back_buffer ? back_buffer : fb.buffer;
}
