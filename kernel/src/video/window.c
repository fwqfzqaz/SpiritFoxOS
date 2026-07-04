/*
 * window.c - Graphical window manager for SpiritFoxOS
 * Multi-window, start menu, desktop icons, right-click menu
 *
 * Enter via shell command "window", exit with Escape
 */

#include "window.h"
#include "fb.h"
#include "vga.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "string.h"
#include "boot.h"
#include "logo_data.h"

extern BootInfo *g_boot_info;

/* ================================================================
 *  COLOR SCHEME - Unified blue-based palette
 * ================================================================ */

/* Extended palette for window manager */
#define COLOR_PRIMARY       0x002860B0   /* Main blue */
#define COLOR_PRIMARY_LIGHT 0x003878CC   /* Lighter blue */
#define COLOR_PRIMARY_DARK  0x001C4480   /* Darker blue */
#define COLOR_ACCENT        0x0044A0E0   /* Bright accent blue */
#define COLOR_BG_TOP        0x000C1628   /* Darker at top */
#define COLOR_BG_BOT        0x00142240   /* Lighter at bottom */
#define COLOR_SURFACE       0x00182238   /* Card/window surface */
#define COLOR_SURFACE_LIGHT 0x00203050   /* Lighter surface (hover) */
#define COLOR_SURFACE_DIM   0x00101828   /* Dimmer surface */
#define COLOR_TEXT_BRIGHT   0x00FFFFFF   /* Bright text */
#define COLOR_TEXT          FB_COLOR_TEXT
#define COLOR_TEXT_DIM      FB_COLOR_TEXT_DIM
#define COLOR_TASKBAR_LINE  0x00306090   /* Taskbar accent line */
#define COLOR_SUCCESS       0x0028A050   /* Green */
#define COLOR_WARNING       0x00D09020   /* Orange */
#define COLOR_DANGER        0x00C04040   /* Red */

/* ================================================================
 *  APP DEFINITIONS
 * ================================================================ */

#define NUM_APPS 7
/* 0-4: main apps, 5: Home, 6: Trash */

static const char *app_names[NUM_APPS] = {
    "Terminal",       /* 0 */
    "File Manager",   /* 1 */
    "System Monitor", /* 2 */
    "Settings",       /* 3 */
    "About",          /* 4 */
    "Home",           /* 5 */
    "Trash"           /* 6 */
};

/* ================================================================
 *  MULTI-WINDOW STATE
 * ================================================================ */

#define MAX_WINDOWS 8

typedef struct {
    int app;
    int x, y, w, h;
    int minimized;
    int maximized;
    int saved_x, saved_y, saved_w, saved_h;
} WinSlot;

static WinSlot wins[MAX_WINDOWS];
static int num_wins;
static int focus_win;  /* -1 = none */

/* ================================================================
 *  MOUSE & INPUT STATE
 * ================================================================ */

static int mouse_x = 0, mouse_y = 0;
static int mouse_prev_x = 0, mouse_prev_y = 0;
static uint8_t mouse_buttons = 0;
static uint8_t mouse_prev_buttons = 0;

static int dragging = 0;
static int drag_win = -1;
static int drag_off_x = 0, drag_off_y = 0;

/* Resizing state */
static int resizing = 0;
static int resize_win = -1;
static int resize_edge = 0;  /* bitmask: 1=right, 2=bottom, 4=left, 8=top */
static int resize_start_x = 0, resize_start_y = 0;
static int resize_orig_w = 0, resize_orig_h = 0;
static int resize_orig_x = 0, resize_orig_y = 0;

/* Start menu */
static int start_menu_open = 0;
static int start_menu_hover = -1;

/* Desktop context menu */
static int ctx_menu_open = 0;
static int ctx_menu_x = 0, ctx_menu_y = 0;
static int ctx_menu_hover = -1;

/* Icon press feedback */
static int icon_pressed = -1;  /* index of currently pressed icon, -1=none */

/* ================================================================
 *  LAYOUT CONSTANTS
 * ================================================================ */

#define TASKBAR_HEIGHT    44
#define TITLE_BAR_HEIGHT  30
#define WIN_SHADOW_OFFSET 4
#define WIN_BODY_COLOR    COLOR_SURFACE
#define WIN_SHADOW_COLOR  0x00060A14

/* Desktop icon grid */
#define ICON_SIZE         40
#define ICON_CELL_W       80
#define ICON_CELL_H       86
#define ICON_GRID_LEFT    16
#define ICON_GRID_TOP     16
#define ICON_GRID_COLS    6

/* ================================================================
 *  TERMINAL EMULATOR
 * ================================================================ */

#define TERM_BUF_LINES 50
#define TERM_BUF_COLS  80
static char term_buf[TERM_BUF_LINES][TERM_BUF_COLS + 1];
static int term_cursor_line;
static int term_cursor_col;
static char term_input[TERM_BUF_COLS];
static int term_input_len;

static void term_clear(void)
{
    for (int i = 0; i < TERM_BUF_LINES; i++) {
        for (int j = 0; j < TERM_BUF_COLS; j++)
            term_buf[i][j] = ' ';
        term_buf[i][TERM_BUF_COLS] = '\0';
    }
    term_cursor_line = 0;
    term_cursor_col = 0;
}

static void term_print(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            term_cursor_col = 0;
            term_cursor_line++;
            if (term_cursor_line >= TERM_BUF_LINES) {
                for (int i = 0; i < TERM_BUF_LINES - 1; i++)
                    memcpy(term_buf[i], term_buf[i + 1], TERM_BUF_COLS);
                for (int j = 0; j < TERM_BUF_COLS; j++)
                    term_buf[TERM_BUF_LINES - 1][j] = ' ';
                term_cursor_line = TERM_BUF_LINES - 1;
            }
        } else {
            if (term_cursor_col < TERM_BUF_COLS)
                term_buf[term_cursor_line][term_cursor_col++] = *s;
        }
        s++;
    }
}

static void term_execute(const char *cmd)
{
    term_print("> ");
    term_print(cmd);
    term_print("\n");
    if (strcmp(cmd, "help") == 0) {
        term_print("SpiritFoxOS Terminal v0.2\n");
        term_print("Commands: help, clear, version, sysinfo\n");
    } else if (strcmp(cmd, "clear") == 0) {
        term_clear();
    } else if (strcmp(cmd, "version") == 0) {
        term_print("SpiritFoxOS v0.5.0 - Multi-Window GUI\n");
    } else if (strcmp(cmd, "sysinfo") == 0) {
        term_print("Screen: ");
        uint32_t w = fb_get_width(), h = fb_get_height();
        char buf[32]; int pos = 0; int n = (int)w;
        char tmp[12]; int t = 0;
        if (n == 0) buf[pos++] = '0';
        else { while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
            for (int j = t - 1; j >= 0; j--) buf[pos++] = tmp[j]; }
        buf[pos++] = 'x'; n = (int)h; t = 0;
        if (n == 0) buf[pos++] = '0';
        else { while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
            for (int j = t - 1; j >= 0; j--) buf[pos++] = tmp[j]; }
        buf[pos] = '\0';
        term_print(buf);
        term_print(" 32bpp\n");
    } else if (cmd[0] != '\0') {
        term_print("Unknown: ");
        term_print(cmd);
        term_print("\n");
    }
}

/* ================================================================
 *  DRAWING HELPERS
 * ================================================================ */

static void draw_gradient_background(void)
{
    uint32_t sw = fb_get_width(), sh = fb_get_height();
    /* Simple 2-band gradient for performance */
    int mid = (int)sh / 2;
    for (int y = 0; y < mid; y++) {
        /* Interpolate from BG_TOP to a mid color */
        int t = y * 256 / mid;
        int r = (int)(0x0C) + ((0x10 - 0x0C) * t >> 8);
        int g = (int)(0x16) + ((0x1E - 0x16) * t >> 8);
        int b = (int)(0x28) + ((0x38 - 0x28) * t >> 8);
        fb_color_t c = (r << 16) | (g << 8) | b;
        fb_fill_rect(0, y, sw, 1, c);
    }
    for (int y = mid; y < (int)sh; y++) {
        int t = (y - mid) * 256 / ((int)sh - mid);
        int r = (int)(0x10) + ((0x14 - 0x10) * t >> 8);
        int g = (int)(0x1E) + ((0x22 - 0x1E) * t >> 8);
        int b = (int)(0x38) + ((0x40 - 0x38) * t >> 8);
        fb_color_t c = (r << 16) | (g << 8) | b;
        fb_fill_rect(0, y, sw, 1, c);
    }
}

/* Draw rounded rectangle outline */
static void draw_rounded_rect(int x, int y, int w, int h, int r, fb_color_t color)
{
    /* Top and bottom edges (excluding corners) */
    fb_draw_line(x + r, y, x + w - r, y, color);
    fb_draw_line(x + r, y + h - 1, x + w - r, y + h - 1, color);
    /* Left and right edges */
    fb_draw_line(x, y + r, x, y + h - r, color);
    fb_draw_line(x + w - 1, y + r, x + w - 1, y + h - r, color);
    /* Corners - approximate with pixels */
    for (int i = 0; i < r; i++) {
        int dx = r - i - 1;
        /* Top-left */
        fb_put_pixel(x + dx, y + i, color);
        fb_put_pixel(x + i, y + dx, color);
        /* Top-right */
        fb_put_pixel(x + w - 1 - dx, y + i, color);
        fb_put_pixel(x + w - 1 - i, y + dx, color);
        /* Bottom-left */
        fb_put_pixel(x + dx, y + h - 1 - i, color);
        fb_put_pixel(x + i, y + h - 1 - dx, color);
        /* Bottom-right */
        fb_put_pixel(x + w - 1 - dx, y + h - 1 - i, color);
        fb_put_pixel(x + w - 1 - i, y + h - 1 - dx, color);
    }
}

/* Fill rounded rectangle */
static void fill_rounded_rect(int x, int y, int w, int h, int r, fb_color_t color)
{
    /* Top band (with corners) */
    for (int i = 0; i < r; i++) {
        int dx = r - i;
        fb_fill_rect(x + dx, y + i, w - 2 * dx, 1, color);
    }
    /* Middle body */
    fb_fill_rect(x, y + r, w, h - 2 * r, color);
    /* Bottom band */
    for (int i = 0; i < r; i++) {
        int dx = i + 1;
        fb_fill_rect(x + dx, y + h - r + i, w - 2 * dx, 1, color);
    }
}

/* ================================================================
 *  DESKTOP ICONS - Grid layout with detailed pixel art
 * ================================================================ */

static void icon_grid_pos(int idx, int *ix, int *iy)
{
    int col = idx % ICON_GRID_COLS;
    int row = idx / ICON_GRID_COLS;
    *ix = ICON_GRID_LEFT + col * ICON_CELL_W + (ICON_CELL_W - ICON_SIZE) / 2;
    *iy = ICON_GRID_TOP + row * ICON_CELL_H;
}

/* Draw individual icon shapes using pixel art */
static void draw_icon_terminal(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? COLOR_PRIMARY_DARK :
                    hover  ? COLOR_PRIMARY_LIGHT : COLOR_PRIMARY;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 6, bg);

    /* Terminal screen */
    int sx = ix + 6, sy = iy + 6, sw = ICON_SIZE - 12, sh = ICON_SIZE - 16;
    fb_fill_rect(sx, sy, sw, sh, 0x00081018);
    fb_draw_rect(sx, sy, sw, sh, 0x0060A0D0);
    /* Prompt text */
    fb_draw_string(sx + 2, sy + 2, ">_", 0x0040D060, 0x00081018);
    /* Bottom bar */
    fb_fill_rect(ix + 4, iy + ICON_SIZE - 8, ICON_SIZE - 8, 4, 0x00404050);
}

static void draw_icon_folder(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? 0x00406080 :
                    hover  ? 0x006088A0 : 0x00507090;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 6, bg);
    /* Folder tab */
    int fx = ix + 6, fy = iy + 10;
    fb_fill_rect(fx, fy, 14, 4, 0x0080B0D0);
    /* Folder body */
    fb_fill_rect(fx, fy + 3, ICON_SIZE - 12, 20, 0x0080B0D0);
    fb_draw_rect(fx, fy, ICON_SIZE - 12, 23, 0x00A0D0F0);
    /* Inner line */
    fb_draw_line(fx + 2, fy + 8, fx + ICON_SIZE - 14, fy + 8, 0x006090B0);
}

static void draw_icon_sysmon(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? COLOR_PRIMARY_DARK :
                    hover  ? COLOR_PRIMARY_LIGHT : COLOR_PRIMARY;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 6, bg);
    /* Bar chart */
    int bx = ix + 8, by = iy + 8;
    int bh[4] = {12, 20, 16, 24};
    fb_color_t bars[4] = {0x0030A0D0, 0x0020C080, 0x00D0A020, 0x00C04040};
    for (int i = 0; i < 4; i++) {
        int bar_y = by + 26 - bh[i];
        fb_fill_rect(bx + i * 7, bar_y, 5, bh[i], bars[i]);
    }
    /* Axis */
    fb_draw_line(bx - 2, by + 26, bx + 28, by + 26, 0x0080A0B0);
}

static void draw_icon_settings(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? COLOR_PRIMARY_DARK :
                    hover  ? COLOR_PRIMARY_LIGHT : COLOR_PRIMARY;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 6, bg);
    /* Gear icon - simplified with a circle and teeth */
    int cx = ix + ICON_SIZE / 2, cy = iy + ICON_SIZE / 2;
    /* Outer teeth (8 rectangles rotated) */
    for (int a = 0; a < 8; a++) {
        int dx = 8 * ((a & 1) ? 1 : -1);
        int dy = 8 * ((a & 2) ? 1 : -1);
        if (a < 4) {
            fb_fill_rect(cx - 2 + (a % 2 == 0 ? -1 : 1) * 6, cy - 2 + (a < 2 ? -1 : 1) * 6, 5, 5, 0x00B0C0D0);
        }
    }
    /* Center circle */
    fb_fill_rect(cx - 5, cy - 5, 10, 10, 0x00B0C0D0);
    fb_fill_rect(cx - 3, cy - 3, 6, 6, bg);
    /* Teeth extensions */
    fb_fill_rect(cx - 2, cy - 10, 4, 5, 0x00B0C0D0);
    fb_fill_rect(cx - 2, cy + 5, 4, 5, 0x00B0C0D0);
    fb_fill_rect(cx - 10, cy - 2, 5, 4, 0x00B0C0D0);
    fb_fill_rect(cx + 5, cy - 2, 5, 4, 0x00B0C0D0);
}

static void draw_icon_about(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? COLOR_PRIMARY_DARK :
                    hover  ? COLOR_PRIMARY_LIGHT : COLOR_PRIMARY;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 6, bg);
    /* Info circle */
    int cx = ix + ICON_SIZE / 2, cy = iy + ICON_SIZE / 2;
    /* Circle */
    for (int dy = -10; dy <= 10; dy++) {
        for (int dx = -10; dx <= 10; dx++) {
            if (dx * dx + dy * dy <= 100 && dx * dx + dy * dy > 49) {
                int px = cx + dx, py = cy + dy;
                if (px >= ix && px < ix + ICON_SIZE && py >= iy && py < iy + ICON_SIZE)
                    fb_put_pixel(px, py, 0x0060B0E0);
            }
        }
    }
    /* "i" dot and stem */
    fb_fill_rect(cx - 1, cy - 6, 3, 3, 0x0060B0E0);
    fb_fill_rect(cx - 1, cy - 1, 3, 8, 0x0060B0E0);
}

static void draw_icon_home(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? COLOR_PRIMARY_DARK :
                    hover  ? COLOR_PRIMARY_LIGHT : COLOR_PRIMARY;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 6, bg);
    /* House shape */
    int bx = ix + 7, by = iy + 10;
    /* Roof (triangle approximation) */
    for (int i = 0; i < 8; i++) {
        int hw = i + 1;
        int rx = bx + 13 - i;
        fb_fill_rect(rx, by + i, hw * 2, 1, 0x00D0A060);
    }
    /* Body */
    fb_fill_rect(bx + 5, by + 8, 16, 14, 0x0080A0C0);
    fb_draw_rect(bx + 5, by + 8, 16, 14, 0x00A0C0E0);
    /* Door */
    fb_fill_rect(bx + 10, by + 14, 6, 8, 0x00507050);
}

static void draw_icon_trash(int ix, int iy, int hover, int pressed)
{
    fb_color_t bg = pressed ? COLOR_PRIMARY_DARK :
                    hover  ? COLOR_PRIMARY_LIGHT : COLOR_PRIMARY;
    fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, 6, bg);
    /* Trash can */
    int bx = ix + 10, by = iy + 10;
    /* Lid */
    fb_fill_rect(bx - 2, by, 24, 4, 0x00909898);
    fb_fill_rect(bx + 6, by - 4, 8, 4, 0x00909898);
    /* Body */
    fb_fill_rect(bx, by + 4, 20, 18, 0x00707878);
    fb_draw_rect(bx, by + 4, 20, 18, 0x00A0A8A8);
    /* Lines on body */
    fb_draw_line(bx + 5, by + 6, bx + 5, by + 20, 0x00909898);
    fb_draw_line(bx + 10, by + 6, bx + 10, by + 20, 0x00909898);
    fb_draw_line(bx + 15, by + 6, bx + 15, by + 20, 0x00909898);
}

typedef void (*icon_draw_fn)(int, int, int, int);

static const icon_draw_fn icon_drawers[NUM_APPS] = {
    draw_icon_terminal,
    draw_icon_folder,
    draw_icon_sysmon,
    draw_icon_settings,
    draw_icon_about,
    draw_icon_home,
    draw_icon_trash
};

static void draw_desktop_icon(int idx, int hover, int pressed)
{
    int ix, iy;
    icon_grid_pos(idx, &ix, &iy);

    icon_drawers[idx](ix, iy, hover, pressed);

    /* Label below icon, centered */
    const char *name = app_names[idx];
    int len = 0;
    while (name[len]) len++;
    int label_x = ix + (ICON_SIZE - len * 8) / 2;
    if (label_x < 2) label_x = 2;
    int label_y = iy + ICON_SIZE + 4;

    /* Shadow text then bright text */
    fb_draw_string(label_x + 1, label_y + 1, name, 0x00040810, COLOR_BG_TOP);
    fb_draw_string(label_x, label_y, name, hover ? COLOR_TEXT_BRIGHT : COLOR_TEXT, COLOR_BG_TOP);
}

static void draw_all_desktop_icons(int hover_icon)
{
    for (int i = 0; i < NUM_APPS; i++)
        draw_desktop_icon(i, i == hover_icon, i == icon_pressed);
}

/* ================================================================
 *  MOUSE CURSOR
 * ================================================================ */

static void draw_mouse_cursor(int cx, int cy)
{
    static const uint16_t cursor_shape[18] = {
        0x0001, 0x0003, 0x0007, 0x000F, 0x001F, 0x003F,
        0x007F, 0x00FF, 0x01FF, 0x03FF, 0x07FF, 0x0FFF,
        0x07F0, 0x02C0, 0x01C0, 0x0080, 0x0080, 0x0000,
    };
    for (int row = 0; row < 18; row++) {
        uint16_t bits = cursor_shape[row];
        for (int col = 0; col < 12; col++) {
            if (bits & (1 << (11 - col))) {
                int px = cx + col, py = cy + row;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = col + dx, ny = row + dy;
                        int in_shape = 0;
                        if (ny >= 0 && ny < 18 && nx >= 0 && nx < 12)
                            if (cursor_shape[ny] & (1 << (11 - nx))) in_shape = 1;
                        if (!in_shape) {
                            int ox = px + dx, oy = py + dy;
                            if (ox >= 0 && oy >= 0 && ox < (int)fb_get_width() && oy < (int)fb_get_height())
                                fb_put_pixel(ox, oy, 0x00000000);
                        }
                    }
                if (px >= 0 && py >= 0 && px < (int)fb_get_width() && py < (int)fb_get_height())
                    fb_put_pixel(px, py, COLOR_TEXT_BRIGHT);
            }
        }
    }
}

/* ================================================================
 *  TASKBAR - Three zones: Start | Window list | System tray
 * ================================================================ */

#define START_BTN_W  44

static void taskbar_win_rect(int slot, int *bx, int *bw)
{
    int start_x = START_BTN_W + 12;
    *bw = 110;
    *bx = start_x + slot * (*bw + 4);
}

static void draw_taskbar(void)
{
    uint32_t sw = fb_get_width();
    int tb_y = (int)fb_get_height() - TASKBAR_HEIGHT;

    /* Taskbar background */
    fb_fill_rect(0, tb_y, sw, TASKBAR_HEIGHT, FB_COLOR_TASKBAR);
    /* Top accent line */
    fb_draw_line(0, tb_y, (int)sw, tb_y, COLOR_TASKBAR_LINE);
    /* Subtle shadow at top */
    fb_draw_line(0, tb_y + 1, (int)sw, tb_y + 1, 0x00040810);

    /* === LEFT: Start button === */
    {
        int btn_x = 4, btn_y = tb_y + 5;
        int btn_w = START_BTN_W, btn_h = TASKBAR_HEIGHT - 10;
        fb_color_t btn_bg = start_menu_open ? COLOR_PRIMARY_DARK : COLOR_PRIMARY;
        fill_rounded_rect(btn_x, btn_y, btn_w, btn_h, 4, btn_bg);
        /* Fox icon - simple diamond */
        int cx = btn_x + btn_w / 2, cy = btn_y + btn_h / 2;
        fb_fill_rect(cx - 2, cy - 6, 4, 4, COLOR_TEXT_BRIGHT);
        fb_fill_rect(cx - 4, cy - 2, 8, 4, COLOR_TEXT_BRIGHT);
        fb_fill_rect(cx - 6, cy + 2, 4, 4, COLOR_TEXT_BRIGHT);
        fb_fill_rect(cx + 2, cy + 2, 4, 4, COLOR_TEXT_BRIGHT);
        fb_fill_rect(cx - 2, cy + 6, 4, 2, COLOR_TEXT_BRIGHT);
    }

    /* === MIDDLE: Window list === */
    for (int i = 0; i < num_wins; i++) {
        int bx, bw;
        taskbar_win_rect(i, &bx, &bw);
        int by = tb_y + 5;
        int bh = TASKBAR_HEIGHT - 10;
        int is_focused = (i == focus_win) && !wins[i].minimized;
        int is_minimized = wins[i].minimized;

        fb_color_t bg;
        if (is_focused) bg = COLOR_PRIMARY;
        else if (is_minimized) bg = COLOR_SURFACE_DIM;
        else bg = COLOR_SURFACE;

        fill_rounded_rect(bx, by, bw, bh, 3, bg);

        fb_color_t txt = is_focused ? COLOR_TEXT_BRIGHT : COLOR_TEXT_DIM;
        fb_draw_string(bx + 6, by + (bh - 14) / 2, app_names[wins[i].app], txt, bg);

        /* Active indicator */
        if (is_focused)
            fb_fill_rect(bx + 4, by + bh - 3, bw - 8, 2, COLOR_ACCENT);
        else if (!is_minimized)
            fb_fill_rect(bx + 4, by + bh - 3, bw - 8, 2, COLOR_TASKBAR_LINE);
    }

    /* === RIGHT: System tray === */
    {
        int tray_right = (int)sw - 8;
        int tray_y = tb_y + 8;

        /* Time */
        uint64_t ms = timer_get_ms();
        uint64_t sec = ms / 1000;
        uint64_t min = sec / 60;
        sec = sec % 60;
        uint64_t hr = (min / 60) % 24;
        min = min % 60;
        char time_buf[12];
        int pos = 0;
        if (hr < 10) time_buf[pos++] = '0';
        { char tmp[4]; int t = 0; int n = (int)hr;
            if (n == 0) tmp[t++] = '0';
            else while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
            for (int j = t - 1; j >= 0; j--) time_buf[pos++] = tmp[j]; }
        time_buf[pos++] = ':';
        if (min < 10) time_buf[pos++] = '0';
        { char tmp[4]; int t = 0; int n = (int)min;
            if (n == 0) tmp[t++] = '0';
            else while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
            for (int j = t - 1; j >= 0; j--) time_buf[pos++] = tmp[j]; }
        time_buf[pos] = '\0';
        int time_w = pos * 8;
        fb_draw_string(tray_right - time_w, tray_y + 4, time_buf, COLOR_TEXT, FB_COLOR_TASKBAR);

        /* Separator before tray */
        fb_draw_line(tray_right - time_w - 12, tb_y + 6, tray_right - time_w - 12, tb_y + TASKBAR_HEIGHT - 6, COLOR_TASKBAR_LINE);

        /* Simple system indicators (dots) */
        int dot_x = tray_right - time_w - 24;
        fb_fill_rect(dot_x, tray_y + 8, 6, 6, COLOR_SUCCESS);      /* CPU ok */
        fb_fill_rect(dot_x + 10, tray_y + 8, 6, 6, COLOR_WARNING); /* Memory */
    }
}

/* ================================================================
 *  START MENU
 * ================================================================ */

#define START_MENU_W     220
#define START_MENU_ITEM_H 30
#define START_MENU_ITEMS  (NUM_APPS + 2)  /* apps + separator + shutdown */

static int start_menu_top(void)
{
    int tb_y = (int)fb_get_height() - TASKBAR_HEIGHT;
    int menu_h = 40 + NUM_APPS * START_MENU_ITEM_H + 12 + START_MENU_ITEM_H + 8;
    return tb_y - menu_h;
}

static int start_menu_height(void)
{
    return 40 + NUM_APPS * START_MENU_ITEM_H + 12 + START_MENU_ITEM_H + 8;
}

static void draw_start_menu(void)
{
    int tb_y = (int)fb_get_height() - TASKBAR_HEIGHT;
    int menu_h = start_menu_height();
    int menu_x = 4;
    int menu_y = tb_y - menu_h;

    /* Shadow */
    fb_fill_rect(menu_x + 4, menu_y + 4, START_MENU_W, menu_h, 0x00040810);
    /* Background */
    fb_fill_rect(menu_x, menu_y, START_MENU_W, menu_h, 0x00142038);
    draw_rounded_rect(menu_x, menu_y, START_MENU_W, menu_h, 6, COLOR_TASKBAR_LINE);

    /* Header */
    fb_fill_rect(menu_x + 1, menu_y + 1, START_MENU_W - 2, 36, COLOR_PRIMARY);
    fb_draw_string(menu_x + 14, menu_y + 10, "SpiritFoxOS", COLOR_TEXT_BRIGHT, COLOR_PRIMARY);
    fb_draw_string(menu_x + 14, menu_y + 22, "v0.5.0", COLOR_TEXT_DIM, COLOR_PRIMARY);

    /* App items */
    int iy = menu_y + 40;
    for (int i = 0; i < NUM_APPS; i++) {
        int item_y = iy + i * START_MENU_ITEM_H;
        int hovered = (start_menu_hover == i);

        if (hovered)
            fb_fill_rect(menu_x + 4, item_y, START_MENU_W - 8, START_MENU_ITEM_H, COLOR_PRIMARY_LIGHT);

        /* Mini icon */
        int mini_x = menu_x + 14, mini_y = item_y + 5;
        fb_color_t mini_bg = hovered ? COLOR_ACCENT : COLOR_PRIMARY;
        fill_rounded_rect(mini_x, mini_y, 20, 20, 3, mini_bg);
        fb_draw_string(mini_x + 3, mini_y + 4, app_names[i][0] == 'T' ? ">_" :
                       app_names[i][0] == 'F' ? "/" :
                       app_names[i][0] == 'S' && app_names[i][4] == 'M' ? "##" :
                       app_names[i][0] == 'S' ? "[]" :
                       app_names[i][0] == 'A' ? "i" :
                       app_names[i][0] == 'H' ? "^" : "X",
                       COLOR_TEXT_BRIGHT, mini_bg);

        fb_color_t txt = hovered ? COLOR_TEXT_BRIGHT : COLOR_TEXT;
        fb_color_t bg = hovered ? COLOR_PRIMARY_LIGHT : 0x00142038;
        fb_draw_string(menu_x + 40, item_y + 7, app_names[i], txt, bg);
    }

    /* Separator */
    int sep_y = iy + NUM_APPS * START_MENU_ITEM_H + 2;
    fb_draw_line(menu_x + 12, sep_y, menu_x + START_MENU_W - 12, sep_y, COLOR_TASKBAR_LINE);

    /* Shutdown */
    int shutdown_idx = NUM_APPS + 1;
    int shutdown_y = sep_y + 6;
    {
        int hovered = (start_menu_hover == shutdown_idx);
        if (hovered)
            fb_fill_rect(menu_x + 4, shutdown_y, START_MENU_W - 8, START_MENU_ITEM_H, COLOR_DANGER);

        fb_color_t txt = hovered ? COLOR_TEXT_BRIGHT : COLOR_DANGER;
        fb_color_t bg = hovered ? COLOR_DANGER : 0x00142038;
        fb_draw_string(menu_x + 14, shutdown_y + 7, "Shutdown", txt, bg);
    }
}

static int point_in_start_menu(int px, int py)
{
    if (!start_menu_open) return 0;
    int tb_y = (int)fb_get_height() - TASKBAR_HEIGHT;
    int menu_h = start_menu_height();
    return (px >= 4 && px < 4 + START_MENU_W && py >= tb_y - menu_h && py < tb_y);
}

static int start_menu_item_at(int px, int py)
{
    if (!start_menu_open) return -1;
    int menu_y = start_menu_top();
    int iy = menu_y + 40;

    for (int i = 0; i < NUM_APPS; i++) {
        int item_y = iy + i * START_MENU_ITEM_H;
        if (px >= 4 && px < 4 + START_MENU_W && py >= item_y && py < item_y + START_MENU_ITEM_H)
            return i;
    }

    int shutdown_y = iy + NUM_APPS * START_MENU_ITEM_H + 8;
    if (px >= 4 && px < 4 + START_MENU_W && py >= shutdown_y && py < shutdown_y + START_MENU_ITEM_H)
        return NUM_APPS + 1;

    return -1;
}

/* ================================================================
 *  DESKTOP CONTEXT MENU (Right-click)
 * ================================================================ */

#define CTX_MENU_W  180
#define CTX_MENU_ITEM_H 26

static const char *ctx_menu_items[] = {
    "Open Terminal",
    "Open File Manager",
    "System Monitor",
    "Settings",
    "---",            /* separator */
    "About SpiritFoxOS",
    "Shutdown"
};
#define CTX_MENU_COUNT 7

static void draw_ctx_menu(void)
{
    if (!ctx_menu_open) return;
    int mx = ctx_menu_x, my = ctx_menu_y;
    int menu_h = CTX_MENU_COUNT * CTX_MENU_ITEM_H + 8;

    /* Keep on screen */
    if (mx + CTX_MENU_W > (int)fb_get_width()) mx = (int)fb_get_width() - CTX_MENU_W;
    if (my + menu_h > (int)fb_get_height() - TASKBAR_HEIGHT) my = (int)fb_get_height() - TASKBAR_HEIGHT - menu_h;

    /* Shadow + Background */
    fb_fill_rect(mx + 3, my + 3, CTX_MENU_W, menu_h, 0x00040810);
    fb_fill_rect(mx, my, CTX_MENU_W, menu_h, 0x00142038);
    draw_rounded_rect(mx, my, CTX_MENU_W, menu_h, 4, COLOR_TASKBAR_LINE);

    int iy = my + 4;
    for (int i = 0; i < CTX_MENU_COUNT; i++) {
        if (strcmp(ctx_menu_items[i], "---") == 0) {
            /* Separator */
            fb_draw_line(mx + 10, iy + CTX_MENU_ITEM_H / 2, mx + CTX_MENU_W - 10, iy + CTX_MENU_ITEM_H / 2, COLOR_TASKBAR_LINE);
            iy += CTX_MENU_ITEM_H;
            continue;
        }
        int hovered = (ctx_menu_hover == i);
        if (hovered)
            fb_fill_rect(mx + 3, iy, CTX_MENU_W - 6, CTX_MENU_ITEM_H, COLOR_PRIMARY_LIGHT);

        fb_color_t txt = hovered ? COLOR_TEXT_BRIGHT : COLOR_TEXT;
        fb_color_t bg = hovered ? COLOR_PRIMARY_LIGHT : 0x00142038;
        /* Red for shutdown */
        if (i == CTX_MENU_COUNT - 1 && !hovered) txt = COLOR_DANGER;
        fb_draw_string(mx + 12, iy + 5, ctx_menu_items[i], txt, bg);
        iy += CTX_MENU_ITEM_H;
    }
}

static int point_in_ctx_menu(int px, int py)
{
    if (!ctx_menu_open) return 0;
    int menu_h = CTX_MENU_COUNT * CTX_MENU_ITEM_H + 8;
    int mx = ctx_menu_x, my = ctx_menu_y;
    if (mx + CTX_MENU_W > (int)fb_get_width()) mx = (int)fb_get_width() - CTX_MENU_W;
    if (my + menu_h > (int)fb_get_height() - TASKBAR_HEIGHT) my = (int)fb_get_height() - TASKBAR_HEIGHT - menu_h;
    return (px >= mx && px < mx + CTX_MENU_W && py >= my && py < my + menu_h);
}

static int ctx_menu_item_at(int px, int py)
{
    if (!ctx_menu_open) return -1;
    int mx = ctx_menu_x, my = ctx_menu_y;
    int menu_h = CTX_MENU_COUNT * CTX_MENU_ITEM_H + 8;
    if (mx + CTX_MENU_W > (int)fb_get_width()) mx = (int)fb_get_width() - CTX_MENU_W;
    if (my + menu_h > (int)fb_get_height() - TASKBAR_HEIGHT) my = (int)fb_get_height() - TASKBAR_HEIGHT - menu_h;

    int iy = my + 4;
    for (int i = 0; i < CTX_MENU_COUNT; i++) {
        if (px >= mx && px < mx + CTX_MENU_W && py >= iy && py < iy + CTX_MENU_ITEM_H)
            return i;
        iy += CTX_MENU_ITEM_H;
    }
    return -1;
}

/* ================================================================
 *  WINDOW FRAME
 * ================================================================ */

static void draw_window_frame(int x, int y, int w, int h, const char *title,
                              int active, int maximized)
{
    fb_color_t border = active ? COLOR_ACCENT : COLOR_TASKBAR_LINE;
    fb_color_t title_bg = active ? COLOR_PRIMARY : COLOR_SURFACE;

    /* Shadow */
    fb_fill_rect(x + WIN_SHADOW_OFFSET, y + WIN_SHADOW_OFFSET, w, h, WIN_SHADOW_COLOR);

    /* Body */
    fb_fill_rect(x, y + TITLE_BAR_HEIGHT, w, h - TITLE_BAR_HEIGHT, WIN_BODY_COLOR);

    /* Title bar */
    fb_fill_rect(x, y, w, TITLE_BAR_HEIGHT, title_bg);

    /* Rounded corners */
    int cr = 5;
    for (int i = 0; i < cr; i++) {
        int dx = cr - i;
        /* Clear corners with bg color (approximate) */
        fb_fill_rect(x, y + i, dx, 1, COLOR_BG_TOP);
        fb_fill_rect(x + w - dx, y + i, dx, 1, COLOR_BG_TOP);
        fb_fill_rect(x, y + h - 1 - i, dx, 1, COLOR_BG_TOP);
        fb_fill_rect(x + w - dx, y + h - 1 - i, dx, 1, COLOR_BG_TOP);
    }

    /* Title text */
    fb_draw_string(x + 12, y + 8, title, COLOR_TEXT_BRIGHT, title_bg);

    /* Control buttons */
    int btn_size = 22;
    int btn_y = y + 4;
    int btn_right = x + w - 8;
    int min_x = btn_right - btn_size * 3 - 6 * 2;
    int max_x = btn_right - btn_size * 2 - 6;
    int close_x = btn_right - btn_size;

    /* Minimize */
    fill_rounded_rect(min_x, btn_y, btn_size, btn_size, 3, COLOR_SURFACE);
    fb_draw_line(min_x + 5, btn_y + btn_size - 6, min_x + btn_size - 5, btn_y + btn_size - 6, COLOR_TEXT);

    /* Maximize / Restore */
    fill_rounded_rect(max_x, btn_y, btn_size, btn_size, 3, COLOR_SURFACE);
    if (maximized) {
        /* Restore icon: two overlapping rectangles */
        fb_draw_rect(max_x + 5, btn_y + 4, 9, 9, COLOR_TEXT);
        fb_draw_rect(max_x + 8, btn_y + 7, 9, 9, COLOR_TEXT);
        fb_fill_rect(max_x + 9, btn_y + 8, 7, 7, COLOR_SURFACE);
    } else {
        fb_draw_rect(max_x + 5, btn_y + 4, 12, 12, COLOR_TEXT);
    }

    /* Close */
    fill_rounded_rect(close_x, btn_y, btn_size, btn_size, 3, COLOR_DANGER);
    fb_draw_string(close_x + 5, btn_y + 3, "X", COLOR_TEXT_BRIGHT, COLOR_DANGER);

    /* Border */
    fb_draw_rect(x, y, w, h, border);

    /* Resize handle (bottom-right corner) - 3 diagonal lines */
    if (!maximized) {
        int rx = x + w - 3, ry = y + h - 3;
        fb_put_pixel(rx, ry, border);
        fb_put_pixel(rx - 4, ry, border); fb_put_pixel(rx, ry - 4, border);
        fb_put_pixel(rx - 8, ry, border); fb_put_pixel(rx, ry - 8, border);
        fb_put_pixel(rx - 4, ry - 4, border);
    }
}

/* ================================================================
 *  APPLICATION WINDOW DRAWING
 * ================================================================ */

static void draw_terminal_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "Terminal",
                      ws == &wins[focus_win], ws->maximized);
    int tx = ws->x + 10, ty = ws->y + TITLE_BAR_HEIGHT + 6;
    int term_rows = (ws->h - TITLE_BAR_HEIGHT - 40) / 16;
    if (term_rows > TERM_BUF_LINES) term_rows = TERM_BUF_LINES;
    int start_line = term_cursor_line - term_rows + 1;
    if (start_line < 0) start_line = 0;
    for (int i = 0; i < term_rows; i++) {
        int li = start_line + i;
        if (li >= 0 && li < TERM_BUF_LINES)
            fb_draw_string(tx, ty + i * 16, term_buf[li], COLOR_SUCCESS, WIN_BODY_COLOR);
    }
    int input_y = ty + term_rows * 16;
    fb_draw_string(tx, input_y, "> ", 0x0040A0C0, WIN_BODY_COLOR);
    fb_draw_string(tx + 16, input_y, term_input, COLOR_TEXT_BRIGHT, WIN_BODY_COLOR);
    if ((timer_get_ms() / 500) % 2 == 0) {
        int cursor_x = tx + 16 + term_input_len * 8;
        fb_fill_rect(cursor_x, input_y, 8, 16, COLOR_TEXT_BRIGHT);
    }
}

static void draw_sysmon_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "System Monitor",
                      ws == &wins[focus_win], ws->maximized);
    int tx = ws->x + 14, ty = ws->y + TITLE_BAR_HEIGHT + 10;
    fb_draw_string(tx, ty, "SpiritFoxOS System Monitor", COLOR_TEXT_BRIGHT, WIN_BODY_COLOR); ty += 28;

    fb_draw_string(tx, ty, "Resolution:", COLOR_TEXT_DIM, WIN_BODY_COLOR);
    { char buf[32]; int pos = 0; uint32_t w = fb_get_width(), h = fb_get_height();
        int n = (int)w; char tmp[12]; int t = 0;
        if (n == 0) buf[pos++] = '0';
        else { while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
            for (int j = t - 1; j >= 0; j--) buf[pos++] = tmp[j]; }
        buf[pos++] = 'x'; n = (int)h; t = 0;
        if (n == 0) buf[pos++] = '0';
        else { while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
            for (int j = t - 1; j >= 0; j--) buf[pos++] = tmp[j]; }
        buf[pos++] = 'x'; buf[pos++] = '3'; buf[pos++] = '2'; buf[pos] = '\0';
        fb_draw_string(tx + 120, ty, buf, COLOR_TEXT, WIN_BODY_COLOR); }
    ty += 20;

    fb_draw_string(tx, ty, "Uptime:", COLOR_TEXT_DIM, WIN_BODY_COLOR);
    { char buf[32]; int pos = 0; int n = (int)(timer_get_ms() / 1000);
        char tmp[12]; int t = 0;
        if (n == 0) buf[pos++] = '0';
        else { while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
            for (int j = t - 1; j >= 0; j--) buf[pos++] = tmp[j]; }
        buf[pos++] = 's'; buf[pos] = '\0';
        fb_draw_string(tx + 120, ty, buf, COLOR_TEXT, WIN_BODY_COLOR); }
    ty += 20;

    fb_draw_string(tx, ty, "GUI Mode:", COLOR_TEXT_DIM, WIN_BODY_COLOR);
    fb_draw_string(tx + 120, ty, "Multi-Window", COLOR_SUCCESS, WIN_BODY_COLOR); ty += 20;

    fb_draw_string(tx, ty, "Windows:", COLOR_TEXT_DIM, WIN_BODY_COLOR);
    { char buf[8]; buf[0] = '0' + num_wins; buf[1] = '\0';
        fb_draw_string(tx + 120, ty, buf, COLOR_TEXT, WIN_BODY_COLOR); } ty += 30;

    fb_draw_string(tx, ty, "CPU Usage:", COLOR_TEXT_DIM, WIN_BODY_COLOR);
    int bar_x = tx + 120, bar_w = 160;
    fb_fill_rect(bar_x, ty + 2, bar_w, 12, COLOR_SURFACE_DIM);
    { int usage = (int)((timer_get_ms() / 100) % 80);
        fb_fill_rect(bar_x, ty + 2, bar_w * usage / 100, 12,
                     usage > 60 ? COLOR_WARNING : COLOR_SUCCESS); } ty += 20;

    fb_draw_string(tx, ty, "Memory:", COLOR_TEXT_DIM, WIN_BODY_COLOR);
    fb_fill_rect(bar_x, ty + 2, bar_w, 12, COLOR_SURFACE_DIM);
    fb_fill_rect(bar_x, ty + 2, bar_w * 40 / 100, 12, COLOR_PRIMARY);
}

static void draw_about_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "About SpiritFoxOS",
                      ws == &wins[focus_win], ws->maximized);
    int tx = ws->x + 20, ty = ws->y + TITLE_BAR_HEIGHT + 14;
    fb_draw_string(tx, ty, "SpiritFoxOS v0.5.0", COLOR_TEXT_BRIGHT, WIN_BODY_COLOR); ty += 24;
    fb_draw_string(tx, ty, "Multi-Window Manager", COLOR_TEXT_DIM, WIN_BODY_COLOR); ty += 20;
    fb_draw_string(tx, ty, "Mouse-Supported GUI", 0x0040A0C0, WIN_BODY_COLOR); ty += 20;
    fb_draw_string(tx, ty, "Double-buffered FB", COLOR_TEXT_DIM, WIN_BODY_COLOR); ty += 30;
    fb_draw_string(tx, ty, "Right-click desktop for menu", COLOR_ACCENT, WIN_BODY_COLOR);
}

static void draw_filemanager_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "File Manager",
                      ws == &wins[focus_win], ws->maximized);
    int tx = ws->x + 14, ty = ws->y + TITLE_BAR_HEIGHT + 8;
    fb_draw_string(tx, ty, "Path: /", COLOR_TEXT_BRIGHT, WIN_BODY_COLOR); ty += 22;
    const char *entries[] = {
        "  bin/", "  dev/", "  etc/", "  home/",
        "  mnt/", "  opt/", "  proc/", "  root/",
        "  sbin/", "  sys/", "  tmp/", "  usr/", "  var/"
    };
    for (int i = 0; i < 13; i++) {
        fb_draw_string(tx, ty, entries[i], 0x0040A0C0, WIN_BODY_COLOR);
        ty += 16;
    }
}

static void draw_settings_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "Settings",
                      ws == &wins[focus_win], ws->maximized);
    int tx = ws->x + 18, ty = ws->y + TITLE_BAR_HEIGHT + 14;
    fb_draw_string(tx, ty, "Display", COLOR_TEXT_BRIGHT, WIN_BODY_COLOR); ty += 22;
    fb_draw_string(tx + 16, ty, "Resolution: VBE default", COLOR_TEXT_DIM, WIN_BODY_COLOR); ty += 18;
    fb_draw_string(tx + 16, ty, "Color depth: 32-bit XRGB8888", COLOR_TEXT_DIM, WIN_BODY_COLOR); ty += 28;
    fb_draw_string(tx, ty, "Keyboard", COLOR_TEXT_BRIGHT, WIN_BODY_COLOR); ty += 22;
    fb_draw_string(tx + 16, ty, "Layout: US (Scancode Set 1)", COLOR_TEXT_DIM, WIN_BODY_COLOR); ty += 28;
    fb_draw_string(tx, ty, "Mouse", COLOR_TEXT_BRIGHT, WIN_BODY_COLOR); ty += 22;
    fb_draw_string(tx + 16, ty, "PS/2 compatible, 3 buttons", COLOR_TEXT_DIM, WIN_BODY_COLOR); ty += 28;
    fb_draw_string(tx, ty, "Shortcuts", COLOR_TEXT_BRIGHT, WIN_BODY_COLOR); ty += 22;
    fb_draw_string(tx + 16, ty, "Alt+Tab: Switch window", COLOR_TEXT_DIM, WIN_BODY_COLOR); ty += 18;
    fb_draw_string(tx + 16, ty, "Esc: Exit GUI", COLOR_TEXT_DIM, WIN_BODY_COLOR);
}

/* Home and Trash show simplified content */
static void draw_home_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "Home",
                      ws == &wins[focus_win], ws->maximized);
    int tx = ws->x + 14, ty = ws->y + TITLE_BAR_HEIGHT + 8;
    fb_draw_string(tx, ty, "/home/user", COLOR_TEXT_BRIGHT, WIN_BODY_COLOR); ty += 22;
    const char *items[] = { "  Documents/", "  Downloads/", "  Pictures/", "  Music/", "  Desktop/" };
    for (int i = 0; i < 5; i++) {
        fb_draw_string(tx, ty, items[i], 0x0040A0C0, WIN_BODY_COLOR);
        ty += 18;
    }
}

static void draw_trash_window(WinSlot *ws)
{
    draw_window_frame(ws->x, ws->y, ws->w, ws->h, "Trash",
                      ws == &wins[focus_win], ws->maximized);
    int tx = ws->x + 14, ty = ws->y + TITLE_BAR_HEIGHT + 8;
    fb_draw_string(tx, ty, "Trash is empty", COLOR_TEXT_DIM, WIN_BODY_COLOR);
}

static void draw_app_window(WinSlot *ws)
{
    switch (ws->app) {
    case 0: draw_terminal_window(ws); break;
    case 1: draw_filemanager_window(ws); break;
    case 2: draw_sysmon_window(ws); break;
    case 3: draw_settings_window(ws); break;
    case 4: draw_about_window(ws); break;
    case 5: draw_home_window(ws); break;
    case 6: draw_trash_window(ws); break;
    }
}

/* ================================================================
 *  WINDOW MANAGEMENT
 * ================================================================ */

static void compute_app_size(int app, int *w, int *h)
{
    uint32_t sw = fb_get_width(), sh = fb_get_height();
    switch (app) {
    case 0: *w = sw > 700 ? 680 : (int)sw - 40;
            *h = sh > 500 ? 440 : (int)sh - 100; break;
    case 1: *w = sw > 500 ? 480 : (int)sw - 40;
            *h = sh > 400 ? 360 : (int)sh - 100; break;
    case 2: *w = sw > 500 ? 480 : (int)sw - 40;
            *h = sh > 400 ? 360 : (int)sh - 100; break;
    case 3: *w = sw > 400 ? 380 : (int)sw - 40;
            *h = 340; break;
    case 4: *w = sw > 400 ? 380 : (int)sw - 40;
            *h = 260; break;
    case 5: *w = sw > 400 ? 380 : (int)sw - 40;
            *h = 260; break;
    case 6: *w = sw > 400 ? 380 : (int)sw - 40;
            *h = 200; break;
    default: *w = (int)sw - 40;
             *h = (int)sh - TASKBAR_HEIGHT - 60; break;
    }
}

static int open_window(int app)
{
    if (num_wins >= MAX_WINDOWS) return -1;
    WinSlot *ws = &wins[num_wins];
    ws->app = app;
    ws->minimized = 0;
    ws->maximized = 0;
    compute_app_size(app, &ws->w, &ws->h);
    int offset = num_wins * 28;
    ws->x = ((int)fb_get_width() - ws->w) / 2 + offset;
    ws->y = ((int)fb_get_height() - TASKBAR_HEIGHT - ws->h) / 2 + offset;
    if (ws->x + ws->w > (int)fb_get_width()) ws->x = 0;
    if (ws->y + ws->h > (int)fb_get_height() - TASKBAR_HEIGHT) ws->y = 0;
    focus_win = num_wins;
    num_wins++;
    return focus_win;
}

static void close_window(int slot)
{
    if (slot < 0 || slot >= num_wins) return;
    for (int i = slot; i < num_wins - 1; i++)
        wins[i] = wins[i + 1];
    num_wins--;
    if (focus_win >= num_wins) focus_win = num_wins - 1;
    if (drag_win == slot) { dragging = 0; drag_win = -1; }
    else if (drag_win > slot) drag_win--;
    if (resize_win == slot) { resizing = 0; resize_win = -1; }
    else if (resize_win > slot) resize_win--;
}

/* Check which resize edge the cursor is on for a window */
static int get_resize_edge(WinSlot *ws, int mx, int my)
{
    if (ws->maximized) return 0;
    int edge = 0;
    int margin = 8;
    /* Right edge */
    if (mx >= ws->x + ws->w - margin && mx < ws->x + ws->w + 4 &&
        my >= ws->y && my < ws->y + ws->h)
        edge |= 1;
    /* Bottom edge */
    if (my >= ws->y + ws->h - margin && my < ws->y + ws->h + 4 &&
        mx >= ws->x && mx < ws->x + ws->w)
        edge |= 2;
    /* Left edge */
    if (mx >= ws->x - 4 && mx < ws->x + margin &&
        my >= ws->y && my < ws->y + ws->h)
        edge |= 4;
    /* Top edge */
    if (my >= ws->y - 4 && my < ws->y + margin &&
        mx >= ws->x && mx < ws->x + ws->w)
        edge |= 8;
    return edge;
}

/* ================================================================
 *  MAIN ENTRY
 * ================================================================ */

static void restore_vga_text_mode(void)
{
    fb_term_init();
    printf("Returned from graphical mode.\n/> ");
}

void window_enter(void)
{
    if (!g_boot_info) g_boot_info = (BootInfo *)0x9000;
    fb_init(g_boot_info);
    if (fb_get_width() == 0 || fb_get_height() == 0) return;

    mouse_init();

    /* Show splash logo */
    {
        uint32_t sw = fb_get_width(), sh = fb_get_height();
        fb_clear(COLOR_BG_TOP);
        int logo_x = ((int)sw - LOGO_WIDTH) / 2;
        int logo_y = ((int)sh - LOGO_HEIGHT) / 2 - 20;
        for (int y = 0; y < LOGO_HEIGHT; y++)
            for (int x = 0; x < LOGO_WIDTH; x++) {
                fb_color_t pixel = logo_pixels[y * LOGO_WIDTH + x];
                if (pixel != 0x000000)
                    fb_put_pixel(logo_x + x, logo_y + y, pixel);
            }
        fb_draw_string(((int)sw - 11 * 8) / 2, logo_y + LOGO_HEIGHT + 16,
                       "SpiritFoxOS", COLOR_TEXT_DIM, COLOR_BG_TOP);
        fb_swap_buffer();
        timer_sleep_ms(3000);
    }

    term_clear();
    term_print("SpiritFoxOS Terminal v0.2\n");
    term_print("Type 'help' for commands.\n\n");
    term_input[0] = '\0';
    term_input_len = 0;

    int window_running = 1;
    num_wins = 0;
    focus_win = -1;
    start_menu_open = 0;
    ctx_menu_open = 0;
    icon_pressed = -1;

    { uint8_t btn;
      mouse_get_state(&mouse_x, &mouse_y, &btn);
      mouse_prev_x = mouse_x; mouse_prev_y = mouse_y;
      mouse_prev_buttons = btn; mouse_buttons = btn; }

    /* ======== MAIN LOOP ======== */
    while (window_running) {
        /* ---- Keyboard ---- */
        while (keyboard_has_char()) {
            char c = keyboard_get_char();
            if ((unsigned char)c == 0x1B) { window_running = 0; break; }

            /* Alt+Tab: cycle windows */
            if (c == '\t' && num_wins > 1) {
                focus_win = (focus_win + 1) % num_wins;
                wins[focus_win].minimized = 0;
                continue;
            }

            /* Terminal input */
            if (focus_win >= 0 && wins[focus_win].app == 0) {
                if (c == '\n') {
                    term_input[term_input_len] = '\0';
                    term_execute(term_input);
                    term_input_len = 0;
                    term_input[0] = '\0';
                } else if (c == '\b' || c == 0x7F) {
                    if (term_input_len > 0) term_input[--term_input_len] = '\0';
                } else if (c >= 0x20 && c < 0x7F) {
                    if (term_input_len < TERM_BUF_COLS - 1)
                        term_input[term_input_len++] = c;
                    term_input[term_input_len] = '\0';
                }
            }
        }
        if (!window_running) break;

        /* ---- Mouse ---- */
        mouse_prev_x = mouse_x; mouse_prev_y = mouse_y;
        mouse_prev_buttons = mouse_buttons;
        if (mouse_has_event())
            mouse_get_state(&mouse_x, &mouse_y, &mouse_buttons);

        int left_click = (mouse_buttons & MOUSE_BTN_LEFT) &&
                         !(mouse_prev_buttons & MOUSE_BTN_LEFT);
        int left_down = (mouse_buttons & MOUSE_BTN_LEFT) != 0;
        int right_click = (mouse_buttons & MOUSE_BTN_RIGHT) &&
                          !(mouse_prev_buttons & MOUSE_BTN_RIGHT);

        int tb_y = (int)fb_get_height() - TASKBAR_HEIGHT;

        /* ---- Icon press tracking ---- */
        if (!left_down) icon_pressed = -1;

        /* ---- Start menu hover ---- */
        if (start_menu_open)
            start_menu_hover = start_menu_item_at(mouse_x, mouse_y);
        else
            start_menu_hover = -1;

        /* ---- Context menu hover ---- */
        if (ctx_menu_open)
            ctx_menu_hover = ctx_menu_item_at(mouse_x, mouse_y);
        else
            ctx_menu_hover = -1;

        /* ---- Taskbar click ---- */
        if (left_click && mouse_y >= tb_y) {
            /* Start button */
            if (mouse_x >= 4 && mouse_x < 4 + START_BTN_W &&
                mouse_y >= tb_y + 5 && mouse_y < tb_y + TASKBAR_HEIGHT - 5) {
                start_menu_open = !start_menu_open;
                if (!start_menu_open) start_menu_hover = -1;
                ctx_menu_open = 0;
            } else {
                if (start_menu_open) { start_menu_open = 0; start_menu_hover = -1; }
                ctx_menu_open = 0;
                /* Window buttons */
                int clicked_slot = -1;
                for (int i = 0; i < num_wins; i++) {
                    int bx, bw;
                    taskbar_win_rect(i, &bx, &bw);
                    if (mouse_x >= bx && mouse_x < bx + bw &&
                        mouse_y >= tb_y + 5 && mouse_y < tb_y + TASKBAR_HEIGHT - 5) {
                        clicked_slot = i;
                        break;
                    }
                }
                if (clicked_slot >= 0) {
                    if (clicked_slot == focus_win && !wins[clicked_slot].minimized) {
                        wins[clicked_slot].minimized = 1;
                        if (drag_win == clicked_slot) { dragging = 0; drag_win = -1; }
                    } else {
                        wins[clicked_slot].minimized = 0;
                        focus_win = clicked_slot;
                    }
                }
            }
        }

        /* ---- Start menu click ---- */
        if (left_click && start_menu_open) {
            int item = start_menu_item_at(mouse_x, mouse_y);
            if (item >= 0 && item < NUM_APPS) {
                open_window(item);
                start_menu_open = 0; start_menu_hover = -1;
            } else if (item == NUM_APPS + 1) {
                window_running = 0;
            } else if (!point_in_start_menu(mouse_x, mouse_y)) {
                start_menu_open = 0; start_menu_hover = -1;
            }
        }

        /* ---- Desktop right-click ---- */
        if (right_click && mouse_y < tb_y && !point_in_start_menu(mouse_x, mouse_y)) {
            /* Check not on a window */
            int on_win = 0;
            for (int i = num_wins - 1; i >= 0; i--) {
                if (!wins[i].minimized &&
                    mouse_x >= wins[i].x && mouse_x < wins[i].x + wins[i].w &&
                    mouse_y >= wins[i].y && mouse_y < wins[i].y + wins[i].h) {
                    on_win = 1; break;
                }
            }
            if (!on_win) {
                ctx_menu_open = 1;
                ctx_menu_x = mouse_x;
                ctx_menu_y = mouse_y;
                ctx_menu_hover = -1;
                start_menu_open = 0;
            }
        }

        /* ---- Context menu click ---- */
        if (left_click && ctx_menu_open) {
            int item = ctx_menu_item_at(mouse_x, mouse_y);
            if (item >= 0) {
                switch (item) {
                case 0: open_window(0); break;  /* Terminal */
                case 1: open_window(1); break;  /* File Manager */
                case 2: open_window(2); break;  /* SysMon */
                case 3: open_window(3); break;  /* Settings */
                case 5: open_window(4); break;  /* About */
                case 6: window_running = 0; break;  /* Shutdown */
                }
                ctx_menu_open = 0; ctx_menu_hover = -1;
            } else if (!point_in_ctx_menu(mouse_x, mouse_y)) {
                ctx_menu_open = 0; ctx_menu_hover = -1;
            }
        }

        /* ---- Desktop icon click ---- */
        int click_on_any_window = 0;
        if (left_click && mouse_y < tb_y) {
            if (start_menu_open && !point_in_start_menu(mouse_x, mouse_y)) {
                start_menu_open = 0; start_menu_hover = -1;
            }
            if (ctx_menu_open && !point_in_ctx_menu(mouse_x, mouse_y)) {
                ctx_menu_open = 0; ctx_menu_hover = -1;
            }
            for (int i = num_wins - 1; i >= 0; i--) {
                if (!wins[i].minimized &&
                    mouse_x >= wins[i].x && mouse_x < wins[i].x + wins[i].w &&
                    mouse_y >= wins[i].y && mouse_y < wins[i].y + wins[i].h) {
                    click_on_any_window = 1; break;
                }
            }
        }
        if (left_click && !click_on_any_window && !start_menu_open && !ctx_menu_open && mouse_y < tb_y) {
            for (int i = 0; i < NUM_APPS; i++) {
                int ix, iy;
                icon_grid_pos(i, &ix, &iy);
                if (mouse_x >= ix && mouse_x < ix + ICON_SIZE &&
                    mouse_y >= iy && mouse_y < iy + ICON_SIZE + 18) {
                    icon_pressed = i;
                    open_window(i);
                    break;
                }
            }
        }

        /* ---- Click on windows ---- */
        if (left_click && mouse_y < tb_y && !start_menu_open && !ctx_menu_open) {
            int clicked = -1;

            /* Focused window first */
            if (focus_win >= 0 && !wins[focus_win].minimized) {
                WinSlot *ws = &wins[focus_win];
                if (mouse_x >= ws->x && mouse_x < ws->x + ws->w &&
                    mouse_y >= ws->y && mouse_y < ws->y + ws->h)
                    clicked = focus_win;
            }
            /* Other windows in reverse */
            if (clicked < 0) {
                for (int i = num_wins - 1; i >= 0; i--) {
                    if (i == focus_win || wins[i].minimized) continue;
                    WinSlot *ws = &wins[i];
                    if (mouse_x >= ws->x && mouse_x < ws->x + ws->w &&
                        mouse_y >= ws->y && mouse_y < ws->y + ws->h) {
                        clicked = i; break;
                    }
                }
            }

            if (clicked >= 0 && clicked != focus_win) {
                focus_win = clicked;
                wins[clicked].minimized = 0;
            }

            /* Window button / drag / resize for focused window */
            if (clicked >= 0 && clicked == focus_win) {
                WinSlot *ws = &wins[focus_win];
                int btn_size = 22;
                int btn_y_pos = ws->y + 4;
                int btn_right = ws->x + ws->w - 8;
                int min_btn = btn_right - btn_size * 3 - 6 * 2;
                int max_btn = btn_right - btn_size * 2 - 6;
                int close_btn = btn_right - btn_size;

                /* Title bar buttons */
                if (mouse_y >= btn_y_pos && mouse_y < btn_y_pos + btn_size) {
                    if (mouse_x >= close_btn && mouse_x < close_btn + btn_size) {
                        close_window(focus_win); clicked = -1;
                    } else if (mouse_x >= max_btn && mouse_x < max_btn + btn_size) {
                        if (ws->maximized) {
                            ws->x = ws->saved_x; ws->y = ws->saved_y;
                            ws->w = ws->saved_w; ws->h = ws->saved_h;
                            ws->maximized = 0;
                        } else {
                            ws->saved_x = ws->x; ws->saved_y = ws->y;
                            ws->saved_w = ws->w; ws->saved_h = ws->h;
                            ws->x = 0; ws->y = 0;
                            ws->w = (int)fb_get_width();
                            ws->h = (int)fb_get_height() - TASKBAR_HEIGHT;
                            ws->maximized = 1;
                        }
                        dragging = 0; drag_win = -1;
                    } else if (mouse_x >= min_btn && mouse_x < min_btn + btn_size) {
                        ws->minimized = 1;
                        dragging = 0; drag_win = -1;
                    }
                }

                /* Start dragging if on title bar (not buttons) */
                if (clicked >= 0 && focus_win >= 0 && !wins[focus_win].minimized &&
                    !wins[focus_win].maximized &&
                    mouse_y >= ws->y && mouse_y < ws->y + TITLE_BAR_HEIGHT &&
                    !(mouse_x >= min_btn && mouse_y < btn_y_pos + btn_size)) {
                    dragging = 1; drag_win = focus_win;
                    drag_off_x = mouse_x - ws->x;
                    drag_off_y = mouse_y - ws->y;
                }

                /* Start resizing if on edge */
                if (clicked >= 0 && focus_win >= 0 && !wins[focus_win].minimized &&
                    !wins[focus_win].maximized && !dragging) {
                    int edge = get_resize_edge(ws, mouse_x, mouse_y);
                    if (edge) {
                        resizing = 1; resize_win = focus_win; resize_edge = edge;
                        resize_start_x = mouse_x; resize_start_y = mouse_y;
                        resize_orig_w = ws->w; resize_orig_h = ws->h;
                        resize_orig_x = ws->x; resize_orig_y = ws->y;
                    }
                }
            }
        }

        /* ---- Dragging ---- */
        if (dragging && drag_win >= 0) {
            if (left_down && !wins[drag_win].minimized) {
                WinSlot *ws = &wins[drag_win];
                ws->x = mouse_x - drag_off_x;
                ws->y = mouse_y - drag_off_y;
                int sw = (int)fb_get_width();
                int sh = (int)fb_get_height();
                if (ws->x < 0) ws->x = 0;
                if (ws->y < 0) ws->y = 0;
                if (ws->x + 60 > sw) ws->x = sw - 60;
                if (ws->y + 30 > sh - TASKBAR_HEIGHT) ws->y = sh - TASKBAR_HEIGHT - 30;
            } else {
                dragging = 0; drag_win = -1;
            }
        }

        /* ---- Resizing ---- */
        if (resizing && resize_win >= 0) {
            if (left_down && !wins[resize_win].minimized) {
                WinSlot *ws = &wins[resize_win];
                int dx = mouse_x - resize_start_x;
                int dy = mouse_y - resize_start_y;

                if (resize_edge & 1) { /* right */
                    int nw = resize_orig_w + dx;
                    if (nw >= 160) ws->w = nw;
                }
                if (resize_edge & 2) { /* bottom */
                    int nh = resize_orig_h + dy;
                    if (nh >= 100) ws->h = nh;
                }
                if (resize_edge & 4) { /* left */
                    int nw = resize_orig_w - dx;
                    if (nw >= 160) { ws->w = nw; ws->x = resize_orig_x + dx; }
                }
                if (resize_edge & 8) { /* top */
                    int nh = resize_orig_h - dy;
                    if (nh >= 100) { ws->h = nh; ws->y = resize_orig_y + dy; }
                }
            } else {
                resizing = 0; resize_win = -1;
            }
        }

        /* ---- Draw everything ---- */
        draw_gradient_background();

        /* Desktop icon hover */
        int hover_icon = -1;
        if (mouse_y < tb_y && !start_menu_open && !ctx_menu_open) {
            for (int i = 0; i < NUM_APPS; i++) {
                int ix, iy;
                icon_grid_pos(i, &ix, &iy);
                if (mouse_x >= ix && mouse_x < ix + ICON_SIZE &&
                    mouse_y >= iy && mouse_y < iy + ICON_SIZE + 18) {
                    hover_icon = i; break;
                }
            }
        }
        draw_all_desktop_icons(hover_icon);

        /* Non-focused, non-minimized windows */
        for (int i = 0; i < num_wins; i++) {
            if (i != focus_win && !wins[i].minimized)
                draw_app_window(&wins[i]);
        }
        /* Focused window on top */
        if (focus_win >= 0 && !wins[focus_win].minimized)
            draw_app_window(&wins[focus_win]);

        draw_taskbar();
        if (start_menu_open) draw_start_menu();
        if (ctx_menu_open) draw_ctx_menu();
        draw_mouse_cursor(mouse_x, mouse_y);
        fb_swap_buffer();
        timer_sleep_ms(16);
    }

    restore_vga_text_mode();
}
