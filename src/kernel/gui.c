/* SpiritFoxOS - 图形用户界面
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
#include "gui.h"
#include "fb.h"
#include "font.h"
#include "mouse.h"
#include "keyboard.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "pic.h"
#include "shell.h"
#include "perm.h"
#include "crypto.h"
#include "pci.h"
#include "xhci.h"
#include "usb.h"
#include "ata.h"
#include "sfs.h"
#include "log.h"
#include "splash_logo.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/stdarg.h"

/* ============================================================
 * RTC (Real-Time Clock) - Read from CMOS hardware
 * ============================================================ */

static uint8_t rtc_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static void __attribute__((used)) rtc_get_time(uint8_t *hour, uint8_t *min, uint8_t *sec) {
    /* Wait until RTC update is not in progress */
    while (rtc_read(0x0A) & 0x80) {}

    uint8_t s  = rtc_read(0x00);
    uint8_t m  = rtc_read(0x02);
    uint8_t h  = rtc_read(0x04);
    uint8_t reg_b = rtc_read(0x0B);

    /* Check if values are in BCD format */
    if (!(reg_b & 0x04)) {
        s = bcd_to_bin(s);
        m = bcd_to_bin(m);
        h = bcd_to_bin(h);
    }

    /* Handle 12-hour format */
    if (!(reg_b & 0x02) && (h & 0x80)) {
        h = ((h & 0x7F) % 12) + 12;
    }

    *sec  = s;
    *min  = m;
    *hour = h;
}

/* ============================================================
 * Theme & Color Palette
 * ============================================================ */

/* Desktop background gradient */
#define BG_TOP_R    15
#define BG_TOP_G    15
#define BG_TOP_B    35
#define BG_BOT_R    25
#define BG_BOT_G    30
#define BG_BOT_B    60

/* Taskbar */
#define TB_H            40
#define TB_COLOR        FB_RGB(30, 30, 45)
#define TB_BORDER       FB_RGB(60, 130, 240)
#define TB_BTN_NORMAL   FB_RGB(50, 50, 70)
#define TB_BTN_HOVER    FB_RGB(70, 70, 100)
#define TB_BTN_ACTIVE   FB_RGB(60, 130, 240)
#define TB_TEXT         FB_RGB(220, 220, 230)

/* Window */
#define WIN_TITLE_H     28
#define WIN_BORDER      FB_RGB(60, 130, 240)
#define WIN_TITLE_BG    FB_RGB(40, 40, 60)
#define WIN_TITLE_INACT FB_RGB(55, 55, 65)
#define WIN_BODY        FB_RGB(28, 28, 38)
#define WIN_SHADOW      FB_RGBA(0, 0, 0, 80)
#define WIN_CLOSE       FB_RGB(220, 60, 60)
#define WIN_CLOSE_HOVER FB_RGB(240, 90, 90)

/* Terminal */
#define TERM_FG         FB_RGB(0, 230, 118)
#define TERM_BG         FB_RGB(18, 18, 28)
#define TERM_PROMPT_FG  FB_RGB(100, 200, 255)
#define TERM_CURSOR     FB_RGB(0, 230, 118)

/* Desktop icons */
#define ICON_SIZE       56
#define ICON_LABEL_CLR  FB_RGB(210, 215, 225)
#define ICON_SEL        FB_RGBA(60, 130, 240, 60)

/* ============================================================
 * Data Structures
 * ============================================================ */

/* Terminal buffer */
#define TERM_COLS  128
#define TERM_ROWS  200
static char __attribute__((used)) term_buf[TERM_ROWS][TERM_COLS];
static int __attribute__((used)) term_cursor_x = 0;
static int __attribute__((used)) term_cursor_y = 0;

/* Window states */
typedef struct {
    int     open;       /* 1 = visible (open but may be minimized) */
    int     active;     /* 1 = focused */
    int     minimized;   /* 1 = hidden in taskbar */
    uint32_t z_order;   /* higher = rendered on top */
    int32_t x, y;
    uint32_t w, h;
    int32_t drag_ox, drag_oy;   /* drag offset */
    int     dragging;
    const char *title;
} window_t;

#define MAX_WINDOWS 8
static window_t __attribute__((used)) windows[MAX_WINDOWS];
static uint32_t next_z_order = 0;  /* Monotonically increasing Z counter */

/* ---- Animation system ----
 * Fixed-point (16.16) easing for smooth window transitions.
 * Reference: ToaruOS anim.c, SerenityOS Animation pattern */
typedef enum { ANIM_NONE, ANIM_OPEN, ANIM_CLOSE, ANIM_MINIMIZE, ANIM_RESTORE } anim_type_t;

/* Forward declarations for globals defined later in this file */
extern volatile uint64_t timer_ticks;

typedef struct {
    anim_type_t type;
    int         win_id;
    uint32_t    start_tick;   /* timer_ticks when animation began */
    uint32_t    duration;     /* total ticks (100Hz → 30 ticks ≈ 300ms) */
    /* From / To values */
    int32_t     fx, fy;       /* from position */
    int32_t     tx, ty;       /* to position */
    uint16_t    fw, fh;       /* from size (×256 for fixed-point) */
    uint16_t    tw, th;       /* to size (×256) */
    uint8_t     fa, ta;       /* from/to alpha (0=transparent, 255=opaque) */
    int         done;         /* set when animation completes */
} anim_t;

static anim_t g_anim = { ANIM_NONE };

/* Ease-out cubic: fast start, slow end. t in [0,256], returns [0,256] */
static inline int ease_out_cubic(int t) {
    /* f(t) = 1 - (1-t)^3, scaled to 0..256 */
    int s = 256 - t;           /* (1-t) */
    int s2 = (s * s) >> 8;     /* (1-t)^2  (keep in range) */
    int s3 = (s2 * s) >> 8;    /* (1-t)^3 */
    return 256 - s3;           /* 1 - (1-t)^3 */
}

/* Compute current animation progress. Returns 0 if not active. */
static int anim_update(anim_t *a) {
    if (!a || a->type == ANIM_NONE || a->done) return 0;

    uint32_t elapsed = timer_ticks - a->start_tick;
    if (elapsed >= a->duration) {
        a->done = 1;
        return 256;  /* fully complete */
    }

    /* Linear progress 0..256 */
    int raw = (int)((elapsed * 256) / a->duration);
    if (raw > 256) raw = 256;

    /* Apply easing */
    return ease_out_cubic(raw);
}

/* Start an animation on a window */
static void __attribute__((used)) anim_start(anim_type_t type, int win_id,
                       int32_t fx, int32_t fy, uint16_t fw, uint16_t fh, uint8_t fa,
                       int32_t tx, int32_t ty, uint16_t tw, uint16_t th, uint8_t ta,
                       uint32_t duration_ticks) {
    g_anim.type       = type;
    g_anim.win_id     = win_id;
    g_anim.start_tick = timer_ticks;
    g_anim.duration   = duration_ticks;
    g_anim.fx = fx;  g_anim.fy = fy;
    g_anim.tx = tx;  g_anim.ty = ty;
    g_anim.fw = fw;  g_anim.fh = fh;
    g_anim.tw = tw;  g_anim.th = th;
    g_anim.fa = fa;  g_anim.ta = ta;
    g_anim.done       = 0;
}

/* Get interpolated visual state for the animated window.
 * Returns 1 if window should be drawn this frame, 0 if hidden/transparent. */
static int __attribute__((used)) anim_get_visual(anim_t *a, window_t *vis_out) {
    if (!a || a->type == ANIM_NONE) return 0;
    int progress = anim_update(a);
    if (progress <= 0 && a->type == ANIM_OPEN) return 0;  /* Not visible yet */
    if (a->done && a->type == ANIM_CLOSE) return 0;        /* Fully hidden */

    vis_out->x = a->fx + (((int32_t)(a->tx - a->fx) * progress) >> 8);
    vis_out->y = a->fy + (((int32_t)(a->ty - a->fy) * progress) >> 8);
    vis_out->w = (uint32_t)(((int)a->fw + (((int)(a->tw - a->fw) * progress) >> 8)));
    vis_out->h = (uint32_t)(((int)a->fh + (((int)(a->th - a->fh) * progress) >> 8)));

    /* Clamp minimum size to avoid degenerate draws */
    if (vis_out->w < 20) vis_out->w = 20;
    if (vis_out->h < 10) vis_out->h = 10;

    vis_out->open   = 1;
    vis_out->active = windows[a->win_id].active;
    vis_out->minimized = 0;
    vis_out->title  = windows[a->win_id].title;
    return 1;
}

/* Check if an animation has completed and apply final state.
 * Returns 1 if an animation just completed (caller should set gui_dirty). */
static int anim_finalize(void) {
    if (!g_anim.done || g_anim.type == ANIM_NONE) return 0;

    switch (g_anim.type) {
    case ANIM_CLOSE:
        windows[g_anim.win_id].open = 0;
        windows[g_anim.win_id].active = 0;
        break;
    default:
        break;
    }

    g_anim.type = ANIM_NONE;
    g_anim.done = 0;
    return 1;  /* Signal: state changed, need redraw */
}

/* Window IDs */
#define WIN_TERMINAL  0
#define WIN_SYSTEM    1
#define WIN_ABOUT     2

/* Desktop icon */
typedef struct {
    int32_t  x, y;
    const char *label;
    uint32_t color;
    const char *symbol;
    int      win_id;     /* which window to open */
} desktop_icon_t;

#define MAX_ICONS 8
static desktop_icon_t icons[MAX_ICONS];
static int icon_count = 0;
static int selected_icon = -1;

/* GUI state */
static int gui_running = 1;
static int gui_dirty = 0;  /* Set when windows change, triggers full redraw */

/* ---- FPS Counter ---- */
static uint64_t fps_frame_count = 0;
static uint64_t fps_last_ticks = 0;
static int fps_current = 0;

/* Mouse cursor sprite (16x16 arrow) */
static const uint8_t cursor_sprite[16*2] = {
    0x00,0x00, 0x40,0x00, 0x60,0x00, 0x70,0x00,
    0x78,0x00, 0x7C,0x00, 0x7E,0x00, 0x7F,0x00,
    0x78,0x00, 0x68,0x00, 0x0C,0x00, 0x0C,0x00,
    0x06,0x00, 0x06,0x00, 0x03,0x00, 0x00,0x00
};

/* ============================================================
 * Terminal Output
 * ============================================================ */

static void term_scroll(void) {
    for (int r = 0; r < TERM_ROWS - 1; r++)
        memcpy(term_buf[r], term_buf[r + 1], TERM_COLS);
    memset(term_buf[TERM_ROWS - 1], 0, TERM_COLS);
}

static void term_putchar(char c) {
    if (c == '\n') {
        term_cursor_x = 0;
        term_cursor_y++;
        if (term_cursor_y >= TERM_ROWS) {
            term_cursor_y = TERM_ROWS - 1;
            term_scroll();
        }
    } else if (c == '\b') {
        if (term_cursor_x > 0) {
            term_cursor_x--;
            term_buf[term_cursor_y][term_cursor_x] = 0;
        }
    } else {
        if (term_cursor_x < TERM_COLS - 1) {
            term_buf[term_cursor_y][term_cursor_x] = c;
            term_cursor_x++;
        }
    }
    gui_dirty = 1;  /* Terminal content changed -> trigger redraw */
}

static void gui_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char tmp[512];
    int pos = 0;
    while (*fmt && pos < 511) {
        if (*fmt == '%') {
            fmt++;
            int zeropad = 0, width = 0;
            if (*fmt == '0') { zeropad = 1; fmt++; }
            while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
            if (*fmt == 's') {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s && pos < 511) tmp[pos++] = *s++;
            } else if (*fmt == 'd') {
                int v = va_arg(args, int);
                if (v < 0) { tmp[pos++] = '-'; v = -v; }
                char nb[12]; int ni = 0;
                if (v == 0) nb[ni++] = '0';
                while (v > 0) { nb[ni++] = '0' + v % 10; v /= 10; }
                int pad = width - ni; if (pad > 0) for (int p = 0; p < pad && pos < 511; p++) tmp[pos++] = zeropad ? '0' : ' ';
                while (--ni >= 0 && pos < 511) tmp[pos++] = nb[ni];
            } else if (*fmt == 'u') {
                uint32_t v = (uint32_t)va_arg(args, uint64_t);
                char nb[12]; int ni = 0;
                if (v == 0) nb[ni++] = '0';
                while (v > 0) { nb[ni++] = '0' + v % 10; v /= 10; }
                int pad = width - ni; if (pad > 0) for (int p = 0; p < pad && pos < 511; p++) tmp[pos++] = zeropad ? '0' : ' ';
                while (--ni >= 0 && pos < 511) tmp[pos++] = nb[ni];
            } else if (*fmt == 'x') {
                uint32_t v = (uint32_t)va_arg(args, uint64_t);
                const char hx[] = "0123456789abcdef";
                char nb[9]; int ni = 0;
                if (v == 0) nb[ni++] = '0';
                while (v > 0) { nb[ni++] = hx[v & 0xF]; v >>= 4; }
                int pad = width - ni; if (pad > 0) for (int p = 0; p < pad && pos < 511; p++) tmp[pos++] = zeropad ? '0' : ' ';
                while (--ni >= 0 && pos < 511) tmp[pos++] = nb[ni];
            } else if (*fmt == 'c') {
                tmp[pos++] = (char)va_arg(args, int);
            } else {
                tmp[pos++] = '%';
                if (pos < 511) tmp[pos++] = *fmt;
            }
        } else {
            tmp[pos++] = *fmt;
        }
        fmt++;
    }
    tmp[pos] = '\0';
    va_end(args);

    /* Output to terminal buffer */
    for (int i = 0; i < pos; i++)
        term_putchar(tmp[i]);

    /* Also echo to serial for debugging/testing */
    serial_puts(0x3F8, tmp);

    /* Terminal content changed -> trigger redraw on next frame */
    gui_dirty = 1;
}

/* ============================================================
 * Drawing Primitives — OPTIMIZED
 * ============================================================ */

/* ---- Optimized rounded rect: uses fast fill for body, direct buffer writes for corners ---- */
static void draw_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                               uint32_t r, uint32_t color) {
    /* Clamp radius */
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    /* Main body: two fast fills (horizontal bar + vertical center) */
    fb_fill_rect(x + r, y, w - 2 * r, h, color);       /* Center vertical strip */
    fb_fill_rect(x, y + r, w, h - 2 * r, color);       /* Horizontal band (overlaps center) */

    /* Corners: direct buffer writes with bounds-aware clamping */
    uint32_t *buf = fb_get_draw_buffer();
    uint32_t stride = fb.pitch / 4;
    int sw = (int)fb.width, sh = (int)fb.height;

    for (int dy = 0; dy < (int)r; dy++) {
        for (int dx = 0; dx < (int)r; dx++) {
            int ddx = (int)r - 1 - dx;
            int ddy = (int)r - 1 - dy;
            if (ddx * ddx + ddy * ddy < (int)(r * r)) {
                /* Top-left */
                int px = (int)(x + dx), py = (int)(y + dy);
                if (px >= 0 && px < sw && py >= 0 && py < sh)
                    buf[py * stride + px] = color;
                /* Top-right */
                px = (int)(x + w - 1 - dx);
                if (px >= 0 && px < sw && py >= 0 && py < sh)
                    buf[py * stride + px] = color;
                /* Bottom-left */
                py = (int)(y + h - 1 - dy);
                px = (int)(x + dx);
                if (px >= 0 && px < sw && py >= 0 && py < sh)
                    buf[py * stride + px] = color;
                /* Bottom-right */
                px = (int)(x + w - 1 - dx);
                if (px >= 0 && px < sw && py >= 0 && py < sh)
                    buf[py * stride + px] = color;
            }
        }
    }
}

/* ---- Optimized cursor: direct buffer writes ---- */
static void draw_cursor(int mx, int my) {
    uint32_t *buf = fb_get_draw_buffer();
    uint32_t stride = fb.pitch / 4;
    int sw = (int)fb.width, sh = (int)fb.height;

    for (int row = 0; row < 16; row++) {
        uint8_t xor_byte = cursor_sprite[row * 2];
        for (int col = 0; col < 8; col++) {
            int px = mx + col;
            int py = my + row;
            if (px >= 0 && px < sw && py >= 0 && py < sh) {
                uint8_t xor_bit = (xor_byte >> (7 - col)) & 1;
                if (xor_bit) {
                    buf[py * stride + px] = FB_WHITE;
                }
            }
        }
    }
}

/* ============================================================
 * Desktop Drawing — OPTIMIZED
 * ============================================================ */

static uint32_t *bg_cache = NULL;
static int bg_cache_ready = 0;

static void draw_background(void) {
    /* Build background cache once */
    if (!bg_cache_ready) {
        if (!bg_cache) {
            uint64_t sz = (uint64_t)(fb.height - TB_H) * fb.pitch;
            uint64_t pages = (sz + 4095) / 4096;
            uint64_t phys = pmm_alloc_pages(pages);
            if (phys && phys < 0x100000000ULL) {
                bg_cache = (uint32_t *)phys;
            }
        }
        if (bg_cache) {
            uint32_t h = fb.height - TB_H;
            uint32_t stride = fb.pitch / 4;
            for (uint32_t y = 0; y < h; y++) {
                uint8_t t = (uint8_t)(y * 255 / h);
                uint8_t r = BG_TOP_R + (uint8_t)((int)(BG_BOT_R - BG_TOP_R) * t / 255);
                uint8_t g = BG_TOP_G + (uint8_t)((int)(BG_BOT_G - BG_TOP_G) * t / 255);
                uint8_t b = BG_TOP_B + (uint8_t)((int)(BG_BOT_B - BG_TOP_B) * t / 255);
                uint32_t color = FB_RGB(r, g, b);
                /* Fast fill entire row at once using word stores */
                uint32_t *row = bg_cache + y * stride;
                uint32_t dx = 0;
                uint32_t w4 = fb.width & ~3u;
                for (; dx < w4; dx += 4) {
                    row[dx]     = color;
                    row[dx + 1] = color;
                    row[dx + 2] = color;
                    row[dx + 3] = color;
                }
                for (; dx < fb.width; dx++)
                    row[dx] = color;
            }
            bg_cache_ready = 1;
        }
    }

    if (bg_cache_ready && bg_cache) {
        /* Copy cached background to draw buffer using fast block copy */
        uint64_t total_pixels = (uint64_t)(fb.height - TB_H) * (fb.pitch / 4);
        fb_memcpy32(fb_get_draw_buffer(), bg_cache, total_pixels);
    } else {
        fb_fill_rect(0, 0, fb.width, fb.height - TB_H, FB_RGB(BG_TOP_R, BG_TOP_G, BG_TOP_B));
    }
}

static void draw_taskbar(void) {
    uint32_t y = fb.height - TB_H;

    /* Taskbar background */
    fb_fill_rect_fast(0, y, fb.width, TB_H, TB_COLOR);

    /* Top accent line */
    fb_hline_fast(0, y, fb.width, TB_BORDER);

    /* Start button */
    draw_rounded_rect(6, y + 6, 90, 28, 6, TB_BTN_NORMAL);
    font_draw_string(20, y + 12, "SpiritFox", FB_WHITE, TB_BTN_NORMAL);

    /* Window buttons — start after start button + gap */
    uint32_t btn_x = 104;
    uint32_t btn_w = 100;   /* button width */
    uint32_t btn_gap = 4;   /* gap between buttons */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].open) continue;
        if (btn_x + btn_w > fb.width - 140) break;   /* leave room for FPS + clock */

        uint32_t bc = windows[i].minimized ? TB_BTN_NORMAL
                       : windows[i].active ? TB_BTN_ACTIVE : TB_BTN_NORMAL;
        draw_rounded_rect(btn_x, y + 6, btn_w, 28, 6, bc);

        /* Truncate title to fit inside button (max 11 chars = 88px) */
        char title_buf[12];
        const char *title = windows[i].title;
        int tlen = 0;
        while (title[tlen] && tlen < 11) tlen++;
        for (int ti = 0; ti < tlen; ti++) title_buf[ti] = title[ti];
        if (tlen < 11 && title[tlen]) { title_buf[tlen++] = '.'; title_buf[tlen] = '\0'; }
        else { title_buf[tlen] = '\0'; }
        font_draw_string(btn_x + 6, y + 12, title_buf, FB_WHITE, bc);

        btn_x += btn_w + btn_gap;
    }

    /* FPS display — positioned after window buttons */
    {
        char fps_str[12];
        int fi = 0;
        fps_str[fi++] = 'F'; fps_str[fi++] = 'P'; fps_str[fi++] = 'S'; fps_str[fi++] = ':';
        if (fps_current >= 100) fps_str[fi++] = '0' + (fps_current / 100);
        if (fps_current >= 10)  fps_str[fi++] = '0' + ((fps_current / 10) % 10);
        fps_str[fi++] = '0' + (fps_current % 10);
        fps_str[fi] = '\0';
        font_draw_string(fb.width - 130, y + 12, fps_str, FB_RGB(80, 180, 80), TB_COLOR);
    }

    /* Clock (from RTC) — right-aligned */
    uint8_t rtc_h, rtc_m, rtc_s;
    rtc_get_time(&rtc_h, &rtc_m, &rtc_s);
    char clk[16];
    clk[0] = '0' + (rtc_h / 10); clk[1] = '0' + (rtc_h % 10);
    clk[2] = ':'; clk[3] = '0' + (rtc_m / 10); clk[4] = '0' + (rtc_m % 10);
    clk[5] = ':'; clk[6] = '0' + (rtc_s / 10); clk[7] = '0' + (rtc_s % 10);
    clk[8] = '\0';
    font_draw_string(fb.width - 70, y + 12, clk, TB_TEXT, TB_COLOR);
}

static void draw_desktop_icons(void) {
    for (int i = 0; i < icon_count; i++) {
        desktop_icon_t *ic = &icons[i];
        int32_t ix = ic->x, iy = ic->y;

        /* Selection highlight */
        if (i == selected_icon) {
            fb_fill_rect_fast(ix - 4, iy - 4, ICON_SIZE + 8, ICON_SIZE + 8, ICON_SEL);
        }

        /* Icon box */
        draw_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 10, ic->color);

        /* Symbol */
        int slen = 0;
        const char *s = ic->symbol;
        while (s[slen]) slen++;
        int sx = ix + (ICON_SIZE - slen * FONT_WIDTH) / 2;
        int sy = iy + (ICON_SIZE - FONT_HEIGHT) / 2;
        font_draw_string(sx, sy, ic->symbol, FB_WHITE, ic->color);

        /* Label */
        int llen = 0;
        while (ic->label[llen]) llen++;
        int lx = ix + (ICON_SIZE - llen * FONT_WIDTH) / 2;
        font_draw_string(lx, iy + ICON_SIZE + 4, ic->label, ICON_LABEL_CLR,
                         FB_RGB(BG_BOT_R, BG_BOT_G, BG_BOT_B));
    }
}

/* ============================================================
 * Window Drawing — OPTIMIZED
 * ============================================================ */

static void draw_window(window_t *win) {
    if (!win->open || win->minimized) return;

    /* Guard: during minimize/close animation the window shrinks below usable size.
     * Button coordinates (close_x = x + w - 24) underflow when w < 24.
     * Just draw a minimal rect — content functions also have their own guards. */
    if (win->w < 44 || win->h < WIN_TITLE_H) {
        fb_fill_rect_fast(win->x, win->y, win->w, win->h, WIN_BODY);
        return;
    }

    /* Shadow */
    fb_fill_rect_fast(win->x + 6, win->y + 6, win->w, win->h, FB_RGB(5, 5, 10));

    /* Body */
    fb_fill_rect_fast(win->x, win->y, win->w, win->h, WIN_BODY);

    /* Title bar */
    uint32_t title_bg = win->active ? WIN_TITLE_BG : WIN_TITLE_INACT;
    fb_fill_rect_fast(win->x, win->y, win->w, WIN_TITLE_H, title_bg);

    /* Title bar accent line */
    fb_hline_fast(win->x, win->y + WIN_TITLE_H - 1, win->w, WIN_BORDER);

    /* Title text */
    font_draw_string(win->x + 10, win->y + 6, win->title, FB_WHITE, title_bg);

    /* Close button */
    int close_x = win->x + win->w - 24;
    int close_y = win->y + 4;
    draw_rounded_rect(close_x, close_y, 20, 20, 4, WIN_CLOSE);
    font_draw_string(close_x + 6, close_y + 2, "X", FB_WHITE, WIN_CLOSE);

    /* Minimize button */
    draw_rounded_rect(close_x - 26, close_y, 20, 20, 4, TB_BTN_NORMAL);
    font_draw_string(close_x - 26 + 6, close_y + 2, "_", FB_WHITE, TB_BTN_NORMAL);

    /* Border */
    fb_draw_rect(win->x, win->y, win->w, win->h, WIN_BORDER);
}

/* ---- Optimized Terminal Rendering ----
 * Key optimizations vs original:
 * 1. Single fast-fill for entire terminal background (was per-char bg pixel writes)
 * 2. Direct buffer pointer arithmetic for character rendering
 * 3. Skip fully-blank characters (space + no cursor) to reduce draws by ~50%+
 * 4. Pre-compute row base pointers to avoid repeated multiplication */
static void draw_terminal_window(window_t *win) {
    if (!win->open || win->minimized) return;
    draw_window(win);

    uint32_t cx = win->x + 6;
    uint32_t cy = win->y + WIN_TITLE_H + 4;
    uint32_t cw = win->w - 12;
    uint32_t ch = win->h - WIN_TITLE_H - 10;

    /* Guard: during minimize animation the window shrinks below title height,
     * causing uint32_t underflow in ch → massive out-of-bounds write.
     * Just draw the frame only when content area is too small. */
    if (cw < 2 || ch < 2) return;

    /* Terminal background — single fast fill replaces ~1000+ individual bg writes */
    fb_fill_rect_fast(cx, cy, cw, ch, TERM_BG);

    /* Terminal border */
    fb_draw_rect(cx, cy, cw, ch, FB_RGB(40, 40, 55));

    /* Visible area */
    uint32_t vis_cols = cw / FONT_WIDTH;
    uint32_t vis_rows = ch / FONT_HEIGHT;
    if (vis_cols > TERM_COLS) vis_cols = TERM_COLS;
    if (vis_rows > TERM_ROWS) vis_rows = TERM_ROWS;

    int start_row = term_cursor_y - (int)vis_rows + 1;
    if (start_row < 0) start_row = 0;

    /* Blinking cursor check (once, not per-character) */
    extern volatile uint64_t timer_ticks;
    int cursor_visible = (timer_ticks / 50) % 2 == 0;
    int cr = term_cursor_y - start_row;
    int cursor_in_view = (cr >= 0 && cr < (int)vis_rows);
    uint32_t cursor_px = cx + 2 + (uint32_t)term_cursor_x * FONT_WIDTH;
    uint32_t cursor_py = cy + 2 + (uint32_t)cr * FONT_HEIGHT;

    for (uint32_t r = 0; r < vis_rows; r++) {
        int buf_row = start_row + (int)r;
        if (buf_row < 0 || buf_row >= TERM_ROWS) continue;

        for (uint32_t c = 0; c < vis_cols; c++) {
            char ch2 = term_buf[buf_row][c];
            if (ch2) {
                /* Non-space character: render with fg/bg.
                 * font_draw_char now writes directly to buffer (no function call overhead). */
                font_draw_char(cx + 2 + c * FONT_WIDTH,
                              cy + 2 + r * FONT_HEIGHT,
                              ch2, TERM_FG, TERM_BG);
            }
            /* Space characters: already filled by the background rect above.
             * This is the key optimization — we skip ~50-70% of char renders! */
        }
    }

    /* Blinking cursor — single fast fill */
    if (cursor_visible && cursor_in_view) {
        fb_fill_rect_fast(cursor_px, cursor_py, FONT_WIDTH, FONT_HEIGHT, TERM_CURSOR);
    }
}

static void draw_about_window(window_t *win) {
    if (!win->open || win->minimized) return;
    draw_window(win);

    /* Guard: skip content if window too small (e.g. during minimize animation) */
    if (win->h < WIN_TITLE_H + 30) return;

    uint32_t cx = win->x + 20;
    uint32_t cy = win->y + WIN_TITLE_H + 20;

    /* Fox logo area */
    draw_rounded_rect(cx, cy, 60, 60, 12, FB_SF_ORANGE);
    font_draw_string(cx + 12, cy + 14, "SF", FB_WHITE, FB_SF_ORANGE);

    font_draw_string(cx + 80, cy,      "SpiritFoxOS", FB_WHITE, WIN_BODY);
    font_draw_string(cx + 80, cy + 16, "v1.0.0", FB_RGB(140, 145, 160), WIN_BODY);
    font_draw_string(cx + 80, cy + 32, "x86_64", FB_RGB(140, 145, 160), WIN_BODY);

    font_draw_string(cx, cy + 80,  "Kernel:    SpiritFoxOS-kernel", TERM_FG, WIN_BODY);
    font_draw_string(cx, cy + 96,  "Shell:     sfsh 1.0", TERM_FG, WIN_BODY);
    font_draw_string(cx, cy + 112, "GUI:       SFGUI 2.0", TERM_FG, WIN_BODY);
    font_draw_string(cx, cy + 128, "Security:  Permission Manager", TERM_FG, WIN_BODY);

    char res[64];
    snprintf(res, sizeof(res), "Display:   %ux%u %ubpp", fb.width, fb.height, fb.bpp);
    font_draw_string(cx, cy + 144, res, TERM_FG, WIN_BODY);

    font_draw_string(cx, cy + 180, "Built with love and bare metal.", FB_RGB(100, 100, 120), WIN_BODY);
}

static void draw_system_window(window_t *win) {
    if (!win->open || win->minimized) return;
    draw_window(win);

    /* Guard: skip content if window too small (e.g. during minimize animation) */
    if (win->h < WIN_TITLE_H + 26) return;

    uint32_t cx = win->x + 16;
    uint32_t cy = win->y + WIN_TITLE_H + 16;

    font_draw_string(cx, cy, "System Information", FB_WHITE, WIN_BODY);

    /* Memory */
    uint64_t free_p = pmm_free_count();
    uint64_t total_p = pmm_total_count();
    char mem_str[64];
    snprintf(mem_str, sizeof(mem_str), "Memory: %u MB / %u MB",
             (uint32_t)(free_p * 4 / 1024), (uint32_t)(total_p * 4 / 1024));
    font_draw_string(cx, cy + 24, mem_str, TERM_FG, WIN_BODY);

    /* Uptime */
    extern volatile uint64_t timer_ticks;
    uint32_t secs = (uint32_t)(timer_ticks / 100);
    char up_str[64];
    snprintf(up_str, sizeof(up_str), "Uptime: %u seconds", secs);
    font_draw_string(cx, cy + 40, up_str, TERM_FG, WIN_BODY);

    /* PCI count */
    char pci_str[64];
    snprintf(pci_str, sizeof(pci_str), "PCI Devices: %d", pci_get_device_count());
    font_draw_string(cx, cy + 56, pci_str, TERM_FG, WIN_BODY);

    /* ATA count */
    char ata_str[64];
    snprintf(ata_str, sizeof(ata_str), "ATA Devices: %d", ata_get_device_count());
    font_draw_string(cx, cy + 72, ata_str, TERM_FG, WIN_BODY);

    /* Memory bar */
    uint32_t bar_w = win->w - 40;
    uint32_t bar_h = 16;
    uint32_t bar_y = cy + 100;
    fb_fill_rect_fast(cx, bar_y, bar_w, bar_h, FB_RGB(40, 40, 55));
    uint32_t used = (uint32_t)((total_p - free_p) * 100 / total_p);
    uint32_t fill_w = bar_w * used / 100;
    uint32_t bar_color = used < 60 ? FB_RGB(0, 200, 100) : (used < 85 ? FB_SF_ORANGE : FB_RED);
    if (fill_w > 0) fb_fill_rect_fast(cx, bar_y, fill_w, bar_h, bar_color);
    fb_draw_rect(cx, bar_y, bar_w, bar_h, FB_RGB(60, 60, 80));

    char pct[16];
    snprintf(pct, sizeof(pct), "%u%% used", used);
    font_draw_string(cx + bar_w / 2 - 24, bar_y + 1, pct, FB_WHITE, bar_color);
}

/* ============================================================
 * Window Management (unchanged logic)
 * ============================================================ */

static void win_open(int id, const char *title, uint32_t w, uint32_t h) {
    if (id >= MAX_WINDOWS) return;
    window_t *win = &windows[id];
    win->open = 1;
    win->active = 1;
    win->minimized = 0;
    win->z_order = next_z_order++;
    win->title = title;
    win->w = w;
    win->h = h;

    /* Smart layout: position each window */
    switch (id) {
    case WIN_TERMINAL:
        win->x = 10;  win->y = 25;
        break;
    case WIN_SYSTEM:
        win->x = 520;  win->y = 290;
        break;
    case WIN_ABOUT:
        win->x = 450;  win->y = 160;
        break;
    default:
        win->x = 20 + id * 80;
        win->y = 30 + id * 55;
        break;
    }
    win->dragging = 0;

    /* Deactivate others */
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (i != id) windows[i].active = 0;

    /* Animate: scale up from center with ease-out (300ms = 30 ticks at 100Hz) */
    int32_t cx = win->x + (int32_t)(w / 2);
    int32_t cy = win->y + (int32_t)(h / 2);
    anim_start(ANIM_OPEN, id,
              cx, cy, (uint16_t)(w / 4), (uint16_t)(h / 4), 128,   /* from: small, semi-transparent */
              win->x, win->y, (uint16_t)w, (uint16_t)h, 255,       /* to: full size, opaque */
              30);
}

static void win_close(int id) {
    if (id >= MAX_WINDOWS || !windows[id].open) return;
    window_t *w = &windows[id];

    /* Animate shrink to center then hide (200ms = 20 ticks) */
    int32_t cx = w->x + (int32_t)(w->w / 2);
    int32_t cy = w->y + (int32_t)(w->h / 2);
    anim_start(ANIM_CLOSE, id,
              w->x, w->y, (uint16_t)w->w, (uint16_t)w->h, 255,   /* from: current */
              cx, cy, (uint16_t)(w->w / 4), (uint16_t)(w->h / 4), 64,  /* to: small, fading */
              20);
    /* Don't set open=0 yet — anim_finalize does it when done */
}

static void win_minimize(int id) {
    if (id >= MAX_WINDOWS || !windows[id].open || windows[id].minimized) return;

    windows[id].minimized = 1;
    windows[id].active = 0;
    gui_dirty = 1;
}

static void win_focus(int id) {
    if (id >= MAX_WINDOWS || !windows[id].open) return;
    windows[id].z_order = next_z_order++;
    for (int i = 0; i < MAX_WINDOWS; i++)
        windows[i].active = (i == id);
    gui_dirty = 1;
}

static void win_restore(int id) {
    if (id >= MAX_WINDOWS || !windows[id].open || !windows[id].minimized) return;

    windows[id].minimized = 0;
    win_focus(id);
}

/* Get window ID at given Z position (0=bottom, count-1=top).
 * Only considers open windows. Returns -1 if pos out of range. */
static int get_window_by_z(int pos) {
    uint32_t best_z = 0xFFFFFFFF;
    int best_id = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].open) continue;
        if (windows[i].z_order < best_z) {
            best_z = windows[i].z_order;
            best_id = i;
        }
    }
    for (int p = 0; p < pos && best_id >= 0; p++) {
        uint32_t next_best = 0xFFFFFFFF;
        int next_id = -1;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!windows[i].open) continue;
            if (windows[i].z_order > best_z && windows[i].z_order < next_best) {
                next_best = windows[i].z_order;
                next_id = i;
            }
        }
        best_z = next_best;
        best_id = next_id;
    }
    return best_id;
}

/* Count open windows */
static int count_open_windows(void) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].open) n++;
    return n;
}

static int win_hit_test(int id, int32_t mx, int32_t my) {
    window_t *w = &windows[id];
    if (!w->open || w->minimized) return 0;
    return mx >= w->x && mx < (int32_t)(w->x + w->w) &&
           my >= w->y && my < (int32_t)(w->y + w->h);
}

static int win_close_hit(int id, int32_t mx, int32_t my) {
    window_t *w = &windows[id];
    if (!w->open || w->minimized) return 0;
    int32_t cx = w->x + w->w - 24;
    int32_t cy = w->y + 4;
    return mx >= cx && mx < cx + 20 && my >= cy && my < cy + 20;
}

static int win_minimize_hit(int id, int32_t mx, int32_t my) {
    window_t *w = &windows[id];
    if (!w->open || w->minimized) return 0;
    /* Minimize button is left of close button */
    int32_t cx = w->x + w->w - 24 - 26;
    int32_t cy = w->y + 4;
    return mx >= cx && mx < cx + 20 && my >= cy && my < cy + 20;
}

static int win_titlebar_hit(int id, int32_t mx, int32_t my) {
    window_t *w = &windows[id];
    if (!w->open) return 0;
    return mx >= w->x && mx < (int32_t)(w->x + w->w) &&
           my >= w->y && my < (int32_t)(w->y + WIN_TITLE_H);
}

/* ============================================================
 * Icon Setup
 * ============================================================ */

static void setup_icons(void) {
    icon_count = 0;

    icons[icon_count++] = (desktop_icon_t){
        .x = 30, .y = 40, .label = "Terminal",
        .color = FB_SF_ACCENT, .symbol = ">_", .win_id = WIN_TERMINAL
    };
    icons[icon_count++] = (desktop_icon_t){
        .x = 30, .y = 140, .label = "System",
        .color = FB_SF_ORANGE, .symbol = "SYS", .win_id = WIN_SYSTEM
    };
    icons[icon_count++] = (desktop_icon_t){
        .x = 30, .y = 240, .label = "About",
        .color = FB_RGB(140, 80, 220), .symbol = "i", .win_id = WIN_ABOUT
    };
}

/* ============================================================
 * Terminal Command Handler (unchanged)
 * ============================================================ */

static void term_exec_command(const char *cmd) {
    if (strcmp(cmd, "help") == 0) {
        gui_printf("SpiritFoxOS Shell - Available commands:\n\n");
        gui_printf("  help       Show this help\n");
        gui_printf("  uname      System info\n");
        gui_printf("  uptime     System uptime\n");
        gui_printf("  mem        Memory info\n");
        gui_printf("  clear      Clear terminal\n");
        gui_printf("  neofetch   System info display\n");
        gui_printf("  pcilist    List PCI devices\n");
        gui_printf("  usblist    List USB devices\n");
        gui_printf("  disklist   List ATA disks\n");
        gui_printf("  sfsformat  Format filesystem\n");
        gui_printf("  ls         List files on disk\n");
        gui_printf("  logsave    Save log to disk\n");
        gui_printf("  logload    Load log from disk\n");
        gui_printf("  logauto    Toggle auto-save\n");
        gui_printf("  loglevel   Set log level (0-4)\n");
        gui_printf("  permlist   List all apps & perms\n");
        gui_printf("  shutdown   Save logs and power off\n");
        gui_printf("  reboot     Save logs and reboot\n");
        gui_printf("  exit       Close terminal window\n");
    } else if (strcmp(cmd, "uname") == 0) {
        gui_printf("SpiritFoxOS v1.0.0 x86_64\n");
    } else if (strcmp(cmd, "uptime") == 0) {
        extern volatile uint64_t timer_ticks;
        gui_printf("Uptime: %u seconds\n", (uint32_t)(timer_ticks / 100));
    } else if (strcmp(cmd, "mem") == 0) {
        uint64_t free_p = pmm_free_count();
        uint64_t total_p = pmm_total_count();
        gui_printf("Memory: %u MB free / %u MB total\n",
                   (uint32_t)(free_p * 4 / 1024),
                   (uint32_t)(total_p * 4 / 1024));
    } else if (strcmp(cmd, "clear") == 0) {
        for (int r = 0; r < TERM_ROWS; r++)
            for (int c = 0; c < TERM_COLS; c++)
                term_buf[r][c] = 0;
        term_cursor_x = 0;
        term_cursor_y = 0;
    } else if (strcmp(cmd, "neofetch") == 0) {
        gui_printf("   _____ _    _ _____ _____ ____  \n");
        gui_printf("  / ____| |  | |_   _|  __ \\___ \\ \n");
        gui_printf("  | (___| |  | | | | | |  | |__) |\n");
        gui_printf("   \\___ \\| |  | | | | | |  |  _  < \n");
        gui_printf("   ____) | |__| |_| |_| |  | | \\ \\\n");
        gui_printf("  |_____/ \\____/|_____|_|  |_|  \\_\\\n\n");
        gui_printf("  OS:       SpiritFoxOS v1.0.0 x86_64\n");
        gui_printf("  Kernel:   SpiritFoxOS-kernel\n");
        gui_printf("  Shell:    sfsh 1.0\n");
        gui_printf("  GUI:      SFGUI 2.0\n");
        gui_printf("  Resolution: %ux%u\n", fb.width, fb.height);
        gui_printf("  Security: Permission Manager active\n");
    } else if (strcmp(cmd, "permlist") == 0) {
        gui_printf("ID  Name            Type  State  Permissions\n");
        gui_printf("--  --------------  ----  -----  -----------\n");
        for (int i = 0; i < PERM_MAX_APPS; i++) {
            perm_app_entry_t *a = perm_find_app((uint64_t)(i + 1));
            if (!a || !a->installed) continue;
            const char *type_str = (a->type == APP_SYSTEM) ? "SYS" : "APP";
            const char *state_str = a->active ? "RUN" : "STP";
            gui_printf("%u  %-14s  %s    %s     0x%x\n",
                       (uint32_t)a->app_id, a->name,
                       type_str, state_str, a->granted_perms);
        }
    } else if (strcmp(cmd, "pcilist") == 0) {
        int count = pci_get_device_count();
        gui_printf("Found %d PCI devices:\n", count);
        gui_printf("BUS:DEV.FN VID:DID  Class Sub  ProgIF\n");
        for (int i = 0; i < count; i++) {
            pci_device_t *d = pci_get_device(i);
            if (!d) continue;
            gui_printf("%02x:%02x.%x  %04x:%04x %02x   %02x   %02x",
                       d->bus, d->dev, d->func,
                       d->vendor_id, d->device_id,
                       d->class_code, d->subclass, d->prog_if);
            if (d->class_code == 0x0C && d->subclass == 0x03) {
                const char *t = "?";
                if (d->prog_if == 0x30) t = "xHCI";
                else if (d->prog_if == 0x20) t = "EHCI";
                else if (d->prog_if == 0x00) t = "UHCI";
                else if (d->prog_if == 0x10) t = "OHCI";
                gui_printf(" [%s]", t);
            }
            gui_printf("\n");
        }
    } else if (strcmp(cmd, "usblist") == 0) {
        usb_device_info_t list[USB_MAX_DEVICES];
        int count = usb_get_device_list(list, USB_MAX_DEVICES);
        xhci_controller_t *ctrl = xhci_get_controller();
        if (ctrl) {
            gui_printf("xHCI v%x.%x %d slots %d ports\n",
                       ctrl->hci_version >> 8, ctrl->hci_version & 0xFF,
                       ctrl->max_slots, ctrl->max_ports);
        }
        gui_printf("USB devices: %d\n", count);
        for (int i = 0; i < count; i++) {
            gui_printf("  Slot %d: %s VID=%04x PID=%04x [%s]\n",
                       list[i].slot_id, usb_class_name(list[i].class_code),
                       list[i].vendor_id, list[i].product_id,
                       usb_speed_name(list[i].speed));
        }
        if (count == 0) gui_printf("  (no USB devices detected)\n");
    } else if (strcmp(cmd, "disklist") == 0) {
        int count = ata_get_device_count();
        gui_printf("ATA devices: %d\n", count);
        for (int i = 0; i < count; i++) {
            ata_device_t *d = ata_get_device(i);
            if (!d) continue;
            uint64_t total = d->lba48 ? d->sectors_48 : d->sectors_28;
            uint64_t mb = (total * 512) / (1024 * 1024);
            gui_printf("  %d: %s %s (%u MB)\n",
                       i, d->present ? "Present" : "Absent",
                       d->model, (uint32_t)mb);
        }
        if (count == 0) gui_printf("  (no disks found)\n");
    } else if (strcmp(cmd, "sfsformat") == 0) {
        gui_printf(sfs_format() == 0 ?
                   "Filesystem formatted successfully\n" :
                   "Failed to format filesystem\n");
    } else if (strcmp(cmd, "ls") == 0) {
        if (!sfs_is_formatted()) {
            gui_printf("No filesystem. Use 'sfsformat' first.\n");
        } else {
            sfs_file_entry_t entries[SFS_MAX_FILES];
            int count = sfs_list_files(entries, SFS_MAX_FILES);
            gui_printf("Files: %d\n", count);
            for (int i = 0; i < count; i++)
                gui_printf("  %-32s %u bytes\n", entries[i].name, entries[i].size);
            if (count == 0) gui_printf("  (empty)\n");
        }
    } else if (strcmp(cmd, "logsave") == 0) {
        if (!sfs_is_formatted()) gui_printf("No filesystem. Use 'sfsformat' first.\n");
        else gui_printf(log_save_to_disk() == 0 ? "Log saved to system.log\n" : "Failed to save log\n");
    } else if (strcmp(cmd, "logload") == 0) {
        gui_printf(log_load_from_disk() == 0 ?
                   "Log loaded from disk\n" : "No saved log found\n");
    } else if (strcmp(cmd, "logauto") == 0) {
        int cur = log_auto_save_enabled();
        log_set_auto_save(!cur);
        gui_printf("Auto-save: %s\n", !cur ? "ON" : "OFF");
    } else if (strncmp(cmd, "loglevel ", 9) == 0) {
        int lvl = cmd[9] - '0';
        if (lvl >= 0 && lvl <= 4) {
            log_set_level((log_level_t)lvl);
            gui_printf("Log level set to %s\n", log_level_name((log_level_t)lvl));
        } else {
            gui_printf("Usage: loglevel <0-4>\n");
        }
    } else if (strncmp(cmd, "perminstall ", 12) == 0) {
        char app_name[PERM_NAME_LEN];
        uint32_t req_perms = 0;
        int parsed = 0;
        char *p = (char *)cmd + 12;
        int ni = 0;
        while (*p && *p != ' ' && ni < PERM_NAME_LEN - 1) app_name[ni++] = *p++;
        app_name[ni] = '\0';
        if (*p == ' ') {
            p++;
            while (*p) {
                req_perms *= 16;
                if (*p >= '0' && *p <= '9') req_perms += *p - '0';
                else if (*p >= 'a' && *p <= 'f') req_perms += *p - 'a' + 10;
                else if (*p >= 'A' && *p <= 'F') req_perms += *p - 'A' + 10;
                p++;
            }
            parsed = 1;
        }
        if (parsed) {
            perm_key_t pub_key;
            uint64_t aid = perm_install_app(app_name, req_perms, &pub_key);
            if (aid > 0) {
                gui_printf("Installed '%s' ID=%u perms=0x%x\n", app_name, (uint32_t)aid, req_perms);
            } else {
                gui_printf("Failed to install app\n");
            }
        } else {
            gui_printf("Usage: perminstall <name> <hex_perms>\n");
        }
    } else if (strncmp(cmd, "permstart ", 10) == 0) {
        uint64_t aid = 0;
        char *p = (char *)cmd + 10;
        while (*p >= '0' && *p <= '9') { aid = aid * 10 + (*p - '0'); p++; }
        perm_app_entry_t *app = perm_find_app(aid);
        if (!app) gui_printf("App ID %u not found\n", (uint32_t)aid);
        else if (app->type == APP_SYSTEM) gui_printf("System apps are always active\n");
        else {
            perm_signature_t sig;
            crypto_sign(&app->private_key, aid, &sig);
            perm_session_t session;
            int rc = perm_app_start(aid, &sig, &session);
            gui_printf(rc == 0 ? "App '%s' started\n" : "Failed to start app (err=%d)\n",
                       rc == 0 ? app->name : (const char *)(uint64_t)rc);
        }
    } else if (strncmp(cmd, "permstop ", 9) == 0) {
        uint64_t aid = 0;
        char *p = (char *)cmd + 9;
        while (*p >= '0' && *p <= '9') { aid = aid * 10 + (*p - '0'); p++; }
        gui_printf(perm_app_stop(aid) == 0 ? "App %u stopped\n" : "Failed to stop app\n", (uint32_t)aid);
    } else if (strncmp(cmd, "permcheck ", 10) == 0) {
        uint64_t aid = 0;
        char *p = (char *)cmd + 10;
        while (*p >= '0' && *p <= '9') { aid = aid * 10 + (*p - '0'); p++; }
        if (*p == ' ') p++;
        uint32_t chk = 0;
        while (*p) {
            chk *= 16;
            if (*p >= '0' && *p <= '9') chk += *p - '0';
            else if (*p >= 'a' && *p <= 'f') chk += *p - 'a' + 10;
            else if (*p >= 'A' && *p <= 'F') chk += *p - 'A' + 10;
            p++;
        }
        gui_printf("App %u perm 0x%x: %s\n", (uint32_t)aid, chk,
                   perm_check(aid, (perm_flag_t)chk) ? "GRANTED" : "DENIED");
    } else if (strcmp(cmd, "shutdown") == 0) {
        gui_printf("Saving logs... Shutting down.\n");
        LOG_I("kernel", "System shutdown");
        log_save_to_disk();
        cli();
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        outb(0x64, 0xFE);
        while (1) hlt();
    } else if (strcmp(cmd, "reboot") == 0) {
        gui_printf("Saving logs... Rebooting.\n");
        LOG_I("kernel", "System reboot");
        log_save_to_disk();
        cli();
        outb(0x64, 0xFE);
        while (1) hlt();
    } else {
        gui_printf("sfsh: unknown command: %s\n", cmd);
    }
}

/* ============================================================
 * Splash Screen: display logo for 3 seconds before GUI
 * ============================================================ */
static void show_splash(void) {
    extern volatile uint64_t timer_ticks;

    /* Draw white background + centered logo directly to front buffer */
    uint32_t *screen = fb.buffer;
    uint32_t bg = FB_RGB(255, 255, 255);  /* White background */
    uint32_t stride = fb.pitch / 4;

    /* Logo position: centered on screen */
    int32_t logo_x = ((int32_t)fb.width - SPLASH_WIDTH) / 2;
    int32_t logo_y = ((int32_t)(fb.height - TB_H) - SPLASH_HEIGHT) / 2;

    /* Optimized splash: fill rows instead of per-pixel */
    for (uint32_t y = 0; y < fb.height - TB_H; y++) {
        int32_t ly = (int32_t)y - logo_y;
        uint32_t *row = screen + y * stride;
        if (ly >= 0 && ly < SPLASH_HEIGHT) {
            /* Row intersects logo: copy logo data */
            for (uint32_t x = 0; x < fb.width; x++) {
                int32_t lx = (int32_t)x - logo_x;
                if (lx >= 0 && lx < SPLASH_WIDTH)
                    row[x] = splash_logo[ly * SPLASH_WIDTH + lx];
                else
                    row[x] = bg;
            }
        } else {
            /* Pure background row: fast fill */
            uint32_t dx = 0;
            uint32_t w4 = fb.width & ~3u;
            for (; dx < w4; dx += 4) { row[dx]=bg; row[dx+1]=bg; row[dx+2]=bg; row[dx+3]=bg; }
            for (; dx < fb.width; dx++) row[dx] = bg;
        }
    }

    /* Clear taskbar area with fast fill */
    uint32_t tb_y = fb.height - TB_H;
    for (uint32_t y = tb_y; y < fb.height; y++) {
        uint32_t *row = screen + y * stride;
        uint32_t color = FB_RGB(30, 30, 45);
        uint32_t dx = 0;
        uint32_t w4 = fb.width & ~3u;
        for (; dx < w4; dx += 4) { row[dx]=color; row[dx+1]=color; row[dx+2]=color; row[dx+3]=color; }
        for (; dx < fb.width; dx++) row[dx] = color;
    }

    /* Wait 3 seconds (PIT @100Hz -> 300 ticks) */
    __asm__ volatile("" ::: "memory");
    uint64_t start = timer_ticks;
    while (timer_ticks - start < 300) {
        __asm__ volatile("hlt" ::: "memory");
        __asm__ volatile("" ::: "memory");
    }
}

/* ============================================================
 * Init & Main Loop — OPTIMIZED RENDER PIPELINE
 * ============================================================ */

void gui_init(uint64_t fb_addr, uint32_t fb_width, uint32_t fb_height,
              uint32_t fb_pitch, uint32_t fb_bpp) {
    fb_init(fb_addr, fb_width, fb_height, fb_pitch, fb_bpp);

    /* NOTE: mouse_init() is already called in kernel.c phase_device_selfcheck().
     * Do NOT call it again here — calling it twice corrupts the i8042 keyboard
     * controller state (i8042_flush clears keyboard scan codes, disable/enable
     * keyboard can leave the controller in an inconsistent state), which causes
     * IRQ1 to be stuck and blocks all lower-priority interrupts including PIT. */

    /* Drain any pending keyboard data from the i8042 output buffer.
     * After mouse_init(), the keyboard controller may have leftover scan codes
     * or spurious IRQ1 requests that would block the PIC. */
    cli();
    {
        /* Read and discard all pending data from port 0x60 */
        int max_drain = 16;
        while ((inb(0x64) & 0x01) && max_drain-- > 0) {
            inb(0x60);
            io_wait();
        }
    }
    sti();

    /* Re-register keyboard handler in case it was overwritten */
    keyboard_init();

    /* Ensure PIC IRQ lines are properly unmasked */
    pic_unmask_irq(0);  /* PIT timer */
    pic_unmask_irq(1);  /* Keyboard */
    pic_unmask_irq(2);  /* Cascade */
    pic_unmask_irq(12); /* Mouse */

    /* Clear terminal buffer */
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS; c++)
            term_buf[r][c] = 0;

    /* Setup icons */
    setup_icons();

    /* Init windows */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].open = 0;
        windows[i].active = 0;
    }

    /* Print welcome */
    gui_printf("SpiritFoxOS Terminal v2.0\n");
    gui_printf("SFGUI 2.0 - Type 'help' for commands.\n");
    gui_printf("Shortcut: X then EEE to exit GUI.\n\n");
    gui_printf("root@SpiritFoxOS:~$ ");

    gui_running = 1;

    /* Open terminal window by default */
    win_open(WIN_TERMINAL, "Terminal", 700, 480);
}

void gui_run(void) {
    sti();  /* Ensure interrupts are enabled */
    pic_unmask_irq(0);  /* Re-enable PIT timer interrupt */

    /* Show splash screen: logo for 3 seconds before entering GUI */
    show_splash();

    /* Initialize FPS counter */
    fps_frame_count = 0;
    fps_last_ticks = timer_ticks;
    fps_current = 0;

    char input_buf[256];
    int input_pos = 0;

    /* Shortcut state: x then eee within 1s to force exit GUI */
    int shortcut_phase = 0;
    int shortcut_e_count = 0;
    uint64_t shortcut_deadline = 0;

    /* Auto-demo: execute commands after boot for testing */
    static const char *demo_cmds[] = {
        "help", "uname", "mem", "uptime", "neofetch", "pcilist", "disklist", NULL
    };
    int demo_idx = 0;
    extern volatile uint64_t timer_ticks;
    __asm__ volatile("" ::: "memory");  /* Memory barrier to ensure timer_ticks is fresh */
    uint64_t demo_next = timer_ticks + 50; /* 0.5 seconds after boot */

    /* ---- Mouse input: simple 3-state machine ----
     *   IDLE      — no button pressed
     *   PRESSED   — button just went down (hit-test this frame)
     *   DRAGGING  — button held, window follows cursor
     * Reference: ToaruOS, OSDev bare-bones GUI pattern */
    enum { MOUSE_IDLE, MOUSE_PRESSED, MOUSE_DRAGGING } mouse_state = MOUSE_IDLE;
    int drag_win = -1;
    int32_t drag_ox = 0, drag_oy = 0;  /* cursor-to-window offset */

    /* USB HID poll (low frequency to avoid blocking) */
    {
        static int usb_counter = 0;
        extern int usb_has_mouse(void), usb_has_keyboard(void);
        extern void usb_hid_poll_all(void);
        if (++usb_counter >= 10) {
            usb_counter = 0;
            if (usb_has_mouse() || usb_has_keyboard())
                usb_hid_poll_all();
        }
    }

    while (gui_running) {
        /* ---- Update FPS counter ---- */
        fps_frame_count++;
        {
            uint64_t elapsed = timer_ticks - fps_last_ticks;
            if (elapsed >= 100) {  /* Update every 1 second (100 ticks at 100Hz) */
                fps_current = (int)(fps_frame_count * 100 / elapsed);
                fps_frame_count = 0;
                fps_last_ticks = timer_ticks;
            }
        }

        /* Auto-demo: execute test commands */
        if (demo_cmds[demo_idx] != NULL && timer_ticks >= demo_next) {
            const char *cmd = demo_cmds[demo_idx++];
            gui_printf("%s\n", cmd);
            term_exec_command(cmd);
            gui_printf("root@SpiritFoxOS:~$ ");
            demo_next = timer_ticks + 100; /* 1 second between commands */
        }

        /* ---- Input (non-blocking) ---- */
        uint16_t key = keyboard_try_getkey();
        int has_key = (key != 0xFFFF);

        /* Also check serial port input */
        if (!has_key && serial_has_data(COM1)) {
            char sc = serial_getchar(COM1);
            /* Map serial input to key codes */
            if (sc == '\r' || sc == '\n') key = '\n';
            else if (sc == 0x7F || sc == 0x08) key = '\b';
            else key = (uint16_t)(unsigned char)sc;
            has_key = 1;
        }

        if (has_key) {
            /* Global shortcut: x then eee within 1s -> force exit */
            if (!IS_SPECIAL_KEY(key)) {
                char c = (char)key;
                extern volatile uint64_t timer_ticks;

                if (shortcut_phase == 0) {
                    if (c == 'x' || c == 'X') {
                        shortcut_phase = 1;
                        shortcut_e_count = 0;
                        shortcut_deadline = timer_ticks + 100;
                    }
                } else if (shortcut_phase == 1) {
                    if (timer_ticks > shortcut_deadline) {
                        shortcut_phase = 0;
                    } else if (c == 'e' || c == 'E') {
                        shortcut_e_count++;
                        if (shortcut_e_count >= 3) {
                            LOG_I("gui", "Force exit shortcut triggered");
                            gui_running = 0;
                            break;
                        }
                    } else {
                        shortcut_phase = 0;
                    }
                }
            }

            /* Keyboard input to terminal window */
            if (windows[WIN_TERMINAL].open && windows[WIN_TERMINAL].active) {
                if (!IS_SPECIAL_KEY(key)) {
                    char c = (char)key;
                    if (c == '\n') {
                        input_buf[input_pos] = '\0';
                        gui_printf("\n");
                        if (input_pos > 0) {
                            if (strcmp(input_buf, "exit") == 0 || strcmp(input_buf, "quit") == 0) {
                                win_close(WIN_TERMINAL);
                            } else {
                                term_exec_command(input_buf);
                            }
                        }
                        gui_printf("root@SpiritFoxOS:~$ ");
                        input_pos = 0;
                    } else if (c == '\b') {
                        if (input_pos > 0) {
                            input_pos--;
                            term_putchar('\b');
                        }
                    } else if (c >= ' ' && input_pos < 255) {
                        input_buf[input_pos++] = c;
                        term_putchar(c);
                    }
                }
            }
        }

        /* ---- Mouse: 3-state machine per frame ---- */
        {
            mouse_state_t ms = mouse_get_state();
            int lbtn = (ms.buttons & 0x01) ? 1 : 0;

            switch (mouse_state) {
            case MOUSE_IDLE:
                if (lbtn) {
                    /* Button just pressed — do hit-test once */
                    int32_t mx = ms.x, my = ms.y;
                    int hit = -1;

                    /* Check close button */
                    { int n = count_open_windows();
                      for (int p = n - 1; p >= 0 && hit < 0; p--) {
                          int i = get_window_by_z(p);
                          if (win_close_hit(i, mx, my)) { win_close(i); hit = -2; }
                      }}
                    /* Check minimize button */
                    if (hit >= -1) { int n = count_open_windows();
                      for (int p = n - 1; p >= 0 && hit < 0; p--) {
                          int i = get_window_by_z(p);
                          if (win_minimize_hit(i, mx, my)) { win_minimize(i); hit = -2; }
                      }}
                    /* Check window body / titlebar */
                    if (hit >= -1) { int n = count_open_windows();
                      for (int p = n - 1; p >= 0 && hit < 0; p--) {
                          int i = get_window_by_z(p);
                          if (win_hit_test(i, mx, my)) {
                              win_focus(i);
                              if (win_titlebar_hit(i, mx, my)) {
                                  drag_win = i;
                                  drag_ox = mx - windows[i].x;
                                  drag_oy = my - windows[i].y;
                                  mouse_state = MOUSE_DRAGGING;  /* Skip PRESSED, go straight to drag */
                              } else {
                                  mouse_state = MOUSE_PRESSED;
                              }
                              hit = i;
                          }
                      }}
                    /* Desktop click (icons + taskbar) */
                    if (hit < 0) {
                        selected_icon = -1;
                        for (int i = 0; i < icon_count; i++) {
                            if (mx >= icons[i].x && mx < icons[i].x + ICON_SIZE &&
                                my >= icons[i].y && my < icons[i].y + ICON_SIZE + 16) {
                                selected_icon = i; break;
                            }
                        }
                        if (selected_icon >= 0) {
                            int wid = icons[selected_icon].win_id;
                            if (!windows[wid].open) {
                                switch (wid) {
                                case WIN_TERMINAL: win_open(wid, "Terminal", 700, 480); break;
                                case WIN_SYSTEM:   win_open(wid, "System Monitor", 400, 300); break;
                                case WIN_ABOUT:    win_open(wid, "About SpiritFoxOS", 420, 340); break;
                                }
                            } else { win_focus(wid); }
                        }
                        uint32_t tb_y = fb.height - TB_H;
                        /* Match draw_taskbar() layout: start=104, w=100, gap=4 */
                        uint32_t tb_bx = 104;
                        for (int i = 0; i < MAX_WINDOWS; i++) {
                            if (!windows[i].open) continue;
                            if (tb_bx + 100 > fb.width - 140) break;
                            if (mx >= (int32_t)tb_bx && mx < (int32_t)(tb_bx + 100) &&
                                my >= (int32_t)(tb_y + 6) && my < (int32_t)(tb_y + 34)) {
                                windows[i].minimized ? win_restore(i) : win_focus(i);
                            }
                            tb_bx += 104;   /* 100 width + 4 gap */
                        }
                        gui_dirty = 1;
                        mouse_state = MOUSE_PRESSED;
                    }
                }
                break;

            case MOUSE_PRESSED:
                if (!lbtn) {
                    mouse_state = MOUSE_IDLE;   /* Released without drag */
                } else if (drag_win >= 0) {
                    /* Was in titlebar area, transition to dragging */
                    mouse_state = MOUSE_DRAGGING;
                }
                break;

            case MOUSE_DRAGGING:
                if (!lbtn) {
                    /* Release: end drag */
                    mouse_state = MOUSE_IDLE;
                    drag_win = -1;
                    gui_dirty = 1;
                } else if (drag_win >= 0) {
                    /* Move window with cursor — keep entire window inside screen */
                    int nx = ms.x - drag_ox;
                    int ny = ms.y - drag_oy;
                    /* Clamp: window must stay entirely within desktop area.
                     * fb_fill_rect_fast() has NO bounds checking. */
                    int max_x = (int32_t)(fb.width - windows[drag_win].w);
                    int max_y = (int32_t)(fb.height - TB_H - windows[drag_win].h);
                    if (max_x < 0) max_x = 0;   /* Window wider than screen: pin left */
                    if (max_y < 0) max_y = 0;   /* Window taller than desktop: pin top */
                    if (nx < 0) nx = 0;
                    if (ny < 0) ny = 0;
                    if (nx > max_x) nx = max_x;
                    if (ny > max_y) ny = max_y;
                    if (windows[drag_win].x != nx || windows[drag_win].y != ny) {
                        windows[drag_win].x = nx;
                        windows[drag_win].y = ny;
                        gui_dirty = 1;  /* Trigger full redraw this frame */
                    }
                }
                break;
            }
        }

        /* ---- Render (throttled) ---- */
        {
            static uint64_t last_render = 0;
            static int initial_render_done = 0;
            /* Cursor save/restore: use block copy for 16x16 sprite area */
            static int32_t prev_mx = -1, prev_my = -1;
            static uint32_t cursor_save[256]; /* 16x16 sprite area */
            uint32_t stride = fb.pitch / 4;

            int need_swap = 0;

            /* Finalize any completed animation (apply final state) */
            if (anim_finalize()) gui_dirty = 1;

            /* Always redraw when animation is running */
            int anim_active = (g_anim.type != ANIM_NONE && !g_anim.done);

            if (!initial_render_done || gui_dirty || anim_active) {
                /* Full redraw: first time, state change, or animation frame */
                draw_background();
                draw_desktop_icons();
                {
                    int n = count_open_windows();
                    for (int p = 0; p < n; p++) {
                        int i = get_window_by_z(p);

                        /* If this window is being animated, use its interpolated
                         * visual state instead of the actual window properties */
                        if (anim_active && g_anim.win_id == i) {
                            window_t vis;
                            if (anim_get_visual(&g_anim, &vis)) {
                                switch (i) {
                                case WIN_TERMINAL: draw_terminal_window(&vis); break;
                                case WIN_SYSTEM:   draw_system_window(&vis);   break;
                                case WIN_ABOUT:    draw_about_window(&vis);    break;
                                default:           draw_window(&vis);          break;
                                }
                            }
                            /* else: animation says don't draw (e.g. close done) */
                        } else {
                            /* Normal drawing — skip windows that are mid-close-animation */
                            if (g_anim.type == ANIM_CLOSE && g_anim.win_id == i)
                                continue;
                            switch (i) {
                            case WIN_TERMINAL: draw_terminal_window(&windows[i]); break;
                            case WIN_SYSTEM:   draw_system_window(&windows[i]);   break;
                            case WIN_ABOUT:    draw_about_window(&windows[i]);    break;
                            default:           draw_window(&windows[i]);          break;
                            }
                        }
                    }
                }
                draw_taskbar();

                /* Save area under cursor BEFORE drawing cursor sprite (block copy) */
                {
                    mouse_state_t ms = mouse_get_state();
                    uint32_t *buf = fb_get_draw_buffer();
                    for (int r = 0; r < 16; r++)
                        fb_memcpy32(cursor_save + r * 16,
                                   buf + (ms.y + r) * stride + ms.x, 16);

                    draw_cursor(ms.x, ms.y);
                    prev_mx = ms.x;
                    prev_my = ms.y;
                }
                initial_render_done = 1;
                last_render = timer_ticks;
                if (!anim_active) gui_dirty = 0;  /* Keep dirty while animating */
                need_swap = 1;

            } else {
                /* Clock update every 1 second (100 ticks at 100Hz) */
                if (timer_ticks - last_render >= 100) {
                    last_render = timer_ticks;

                    uint32_t tb_y = fb.height - TB_H;
                    uint8_t rtc_h, rtc_m, rtc_s;
                    rtc_get_time(&rtc_h, &rtc_m, &rtc_s);
                    char clk[16];
                    clk[0] = '0' + (rtc_h / 10);
                    clk[1] = '0' + (rtc_h % 10);
                    clk[2] = ':';
                    clk[3] = '0' + (rtc_m / 10);
                    clk[4] = '0' + (rtc_m % 10);
                    clk[5] = ':';
                    clk[6] = '0' + (rtc_s / 10);
                    clk[7] = '0' + (rtc_s % 10);
                    clk[8] = '\0';
                    fb_fill_rect_fast(fb.width - 80, tb_y + 6, 74, 28, TB_COLOR);
                    font_draw_string(fb.width - 80, tb_y + 12, clk, TB_TEXT, TB_COLOR);

                    need_swap = 1;
                }

                /* Mouse cursor update: erase old position, draw at new position (block copy) */
                {
                    mouse_state_t ms = mouse_get_state();
                    if (ms.x != prev_mx || ms.y != prev_my) {
                        uint32_t *buf = fb_get_draw_buffer();

                        /* Erase old cursor: restore saved pixels using block copy */
                        for (int r = 0; r < 16; r++)
                            fb_memcpy32(buf + (prev_my + r) * stride + prev_mx,
                                       cursor_save + r * 16, 16);

                        /* Save area under new cursor position using block copy */
                        for (int r = 0; r < 16; r++)
                            fb_memcpy32(cursor_save + r * 16,
                                       buf + (ms.y + r) * stride + ms.x, 16);

                        /* Draw cursor at new position */
                        draw_cursor(ms.x, ms.y);

                        prev_mx = ms.x;
                        prev_my = ms.y;
                        need_swap = 1;
                    }
                }  /* end mouse_state_t scope */
            }

            if (need_swap)
                fb_swap_buffers();
        }

        /* Yield CPU until next timer interrupt (~10ms) */
        hlt();
    }
}
