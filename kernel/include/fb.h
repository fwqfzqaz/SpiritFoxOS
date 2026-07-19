#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stddef.h>
#include "boot.h"

/* Framebuffer color (32-bit XRGB8888) */
typedef uint32_t fb_color_t;

/* Common colors */
#define FB_COLOR_BLACK       0x00000000
#define FB_COLOR_WHITE       0x00FFFFFF
#define FB_COLOR_RED         0x00FF0000
#define FB_COLOR_GREEN       0x0000FF00
#define FB_COLOR_BLUE        0x000000FF
#define FB_COLOR_CYAN        0x0000FFFF
#define FB_COLOR_YELLOW      0x00FFFF00
#define FB_COLOR_MAGENTA     0x00FF00FF
#define FB_COLOR_GRAY        0x00808080
#define FB_COLOR_LIGHT_GRAY  0x00C0C0C0
#define FB_COLOR_DARK_GRAY   0x00404040

/* SpiritFoxOS theme colors - Unified blue-based palette */
#define FB_COLOR_BG          0x000C1628   /* Dark navy background */
#define FB_COLOR_TASKBAR     0x000E1828   /* Very dark navy taskbar */
#define FB_COLOR_ACCENT      0x0044A0E0   /* Bright accent blue */
#define FB_COLOR_HIGHLIGHT   0x00C04040   /* Red for close/danger */
#define FB_COLOR_TEXT         0x00D0D8E8   /* Primary text */
#define FB_COLOR_TEXT_DIM     0x00708098   /* Secondary text */

/* Initialize framebuffer from BootInfo */
void fb_init(BootInfo *info);

/* Get framebuffer info */
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);
uint32_t fb_get_pitch(void);
uint32_t fb_get_bpp(void);
void *fb_get_buffer(void);

/* Drawing primitives */
void fb_put_pixel(int x, int y, fb_color_t color);
fb_color_t fb_get_pixel(int x, int y);
void fb_fill_rect(int x, int y, int w, int h, fb_color_t color);
void fb_draw_rect(int x, int y, int w, int h, fb_color_t color);
void fb_draw_line(int x0, int y0, int x1, int y1, fb_color_t color);
void fb_clear(fb_color_t color);

/* Double buffering */
void fb_flip(void);
void fb_flush_rect(int x, int y, int w, int h);

/* Dirty region tracking (for efficient partial updates in GUI mode) */
#define FB_MAX_DIRTY_RECTS  16
void fb_mark_dirty(int x, int y, int w, int h);
void fb_flip_dirty(void);
void fb_clear_dirty(void);

/* Legacy alias */
#define fb_swap_buffer  fb_flip

/* Bitmapped font rendering (8x16 built-in) */
void fb_draw_char(int x, int y, char c, fb_color_t fg, fb_color_t bg);
void fb_draw_string(int x, int y, const char *s, fb_color_t fg, fb_color_t bg);

/* Framebuffer text terminal mode */
void fb_term_init(void);
int  fb_term_is_active(void);
void fb_term_putchar(char c);
void fb_term_get_cursor(int *cx, int *cy);
void fb_term_set_cursor(int cx, int cy);
void fb_term_backspace(void);

#endif /* FB_H */
