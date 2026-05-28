#include "gui.h"
#include "fb.h"
#include "font.h"
#include "mouse.h"
#include "keyboard.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "shell.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/stdarg.h"

/* ============================================================
 * GUI State
 * ============================================================ */

static int gui_running = 1;
static int show_terminal = 0;

/* Terminal buffer */
#define TERM_COLS  128
#define TERM_ROWS  48
static char term_buf[TERM_ROWS][TERM_COLS];
static int term_cursor_x = 0;
static int term_cursor_y = 0;

/* Mouse cursor sprite (16x16 arrow cursor) */
static const uint8_t cursor_sprite[16*2] = {
    0x00,0x00, 0x40,0x00, 0x60,0x00, 0x70,0x00,
    0x78,0x00, 0x7C,0x00, 0x7E,0x00, 0x7F,0x00,
    0x78,0x00, 0x68,0x00, 0x0C,0x00, 0x0C,0x00,
    0x06,0x00, 0x06,0x00, 0x03,0x00, 0x00,0x00
};

/* ============================================================
 * Output helpers
 * ============================================================ */

static void gui_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    /* Write to terminal buffer */
    char tmp[256];
    int pos = 0;
    while (*fmt && pos < 255) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 's') {
                const char *s = va_arg(args, const char *);
                while (*s && pos < 255) tmp[pos++] = *s++;
            } else if (*fmt == 'd') {
                int v = va_arg(args, int);
                if (v < 0) { tmp[pos++] = '-'; v = -v; }
                char nb[12]; int ni = 0;
                if (v == 0) nb[ni++] = '0';
                while (v > 0) { nb[ni++] = '0' + v % 10; v /= 10; }
                while (--ni >= 0 && pos < 255) tmp[pos++] = nb[ni];
            } else if (*fmt == 'u') {
                uint32_t v = (uint32_t)va_arg(args, uint64_t);
                char nb[12]; int ni = 0;
                if (v == 0) nb[ni++] = '0';
                while (v > 0) { nb[ni++] = '0' + v % 10; v /= 10; }
                while (--ni >= 0 && pos < 255) tmp[pos++] = nb[ni];
            } else if (*fmt == 'x') {
                uint32_t v = (uint32_t)va_arg(args, uint64_t);
                const char hx[] = "0123456789ABCDEF";
                char nb[9]; int ni = 0;
                if (v == 0) nb[ni++] = '0';
                while (v > 0) { nb[ni++] = hx[v & 0xF]; v >>= 4; }
                while (--ni >= 0 && pos < 255) tmp[pos++] = nb[ni];
            } else if (*fmt == 'c') {
                tmp[pos++] = (char)va_arg(args, int);
            } else {
                tmp[pos++] = '%';
                tmp[pos++] = *fmt;
            }
        } else {
            tmp[pos++] = *fmt;
        }
        fmt++;
    }
    tmp[pos] = '\0';
    va_end(args);

    /* Write to terminal buffer */
    for (int i = 0; i < pos; i++) {
        if (tmp[i] == '\n') {
            term_cursor_x = 0;
            term_cursor_y++;
            if (term_cursor_y >= TERM_ROWS) {
                term_cursor_y = TERM_ROWS - 1;
                /* Scroll up */
                for (int r = 0; r < TERM_ROWS - 1; r++) {
                    memcpy(term_buf[r], term_buf[r + 1], TERM_COLS);
                }
                memset(term_buf[TERM_ROWS - 1], 0, TERM_COLS);
            }
        } else {
            if (term_cursor_x < TERM_COLS) {
                term_buf[term_cursor_y][term_cursor_x] = tmp[i];
                term_cursor_x++;
            }
        }
    }
}

/* ============================================================
 * Drawing functions
 * ============================================================ */

static void draw_cursor(int mx, int my) {
    for (int row = 0; row < 16; row++) {
        uint8_t and_byte = cursor_sprite[row * 2];
        uint8_t xor_byte = cursor_sprite[row * 2 + 1];
        for (int col = 0; col < 8; col++) {
            int px = mx + col;
            int py = my + row;
            if (px >= 0 && px < (int)fb.width && py >= 0 && py < (int)fb.height) {
                uint8_t and_bit = (and_byte >> (7 - col)) & 1;
                uint8_t xor_bit = (xor_byte >> (7 - col)) & 1;
                if (xor_bit) {
                    fb_draw_pixel(px, py, FB_WHITE);
                } else if (and_bit) {
                    /* transparent */
                }
            }
        }
    }
}

static void draw_desktop(void) {
    /* Background gradient */
    for (uint32_t y = 0; y < fb.height; y++) {
        uint8_t r = (uint8_t)(20 + (y * 10 / fb.height));
        uint8_t g = (uint8_t)(20 + (y * 15 / fb.height));
        uint8_t b = (uint8_t)(40 + (y * 30 / fb.height));
        fb_fill_rect(0, y, fb.width, 1, FB_RGB(r, g, b));
    }

    /* Taskbar at bottom */
    fb_fill_rect(0, fb.height - 36, fb.width, 36, FB_SF_PANEL);
    fb_draw_rect(0, fb.height - 36, fb.width, 1, FB_SF_ACCENT);

    /* Start button */
    fb_fill_rect(4, fb.height - 32, 80, 28, FB_SF_ORANGE);
    fb_draw_rect(4, fb.height - 32, 80, 28, FB_RGB(255, 180, 80));
    font_draw_string(14, fb.height - 26, "SpiritFox", FB_WHITE, FB_SF_ORANGE);

    /* Clock area */
    extern volatile uint64_t timer_ticks;
    uint32_t secs = (uint32_t)(timer_ticks / 100);
    uint32_t mins = secs / 60;
    uint32_t hours = mins / 60;
    char clock_str[16];
    /* Simple clock formatting */
    clock_str[0] = '0' + (hours % 24) / 10;
    clock_str[1] = '0' + (hours % 24) % 10;
    clock_str[2] = ':';
    clock_str[3] = '0' + (mins % 60) / 10;
    clock_str[4] = '0' + (mins % 60) % 10;
    clock_str[5] = '\0';
    font_draw_string(fb.width - 60, fb.height - 26, clock_str, FB_WHITE, FB_SF_PANEL);

    /* System info on taskbar */
    font_draw_string(100, fb.height - 26, "SpiritFoxOS v1.0 | x86_64", FB_LIGHT_GREY, FB_SF_PANEL);
}

static void draw_window(uint32_t wx, uint32_t wy, uint32_t ww, uint32_t wh,
                        const char *title, int active) {
    /* Window shadow */
    fb_fill_rect(wx + 4, wy + 4, ww, wh, FB_RGB(0, 0, 0));

    /* Window background */
    fb_fill_rect(wx, wy, ww, wh, FB_RGB(240, 240, 245));

    /* Title bar */
    uint32_t title_color = active ? FB_SF_ACCENT : FB_GREY;
    fb_fill_rect(wx, wy, ww, 24, title_color);

    /* Title text */
    font_draw_string(wx + 8, wy + 4, title, FB_WHITE, title_color);

    /* Close button */
    fb_fill_rect(wx + ww - 22, wy + 4, 18, 16, FB_RED);
    font_draw_string(wx + ww - 18, wy + 4, "X", FB_WHITE, FB_RED);

    /* Window border */
    fb_draw_rect(wx, wy, ww, wh, FB_DARK_GREY);

    /* Content area border */
    fb_draw_rect(wx, wy + 24, ww, wh - 24, FB_GREY);
}

static void draw_terminal_content(uint32_t wx, uint32_t wy, uint32_t ww, uint32_t wh) {
    uint32_t content_x = wx + 4;
    uint32_t content_y = wy + 28;
    uint32_t visible_rows = (wh - 32) / FONT_HEIGHT;
    uint32_t visible_cols = (ww - 8) / FONT_WIDTH;

    /* Terminal background */
    fb_fill_rect(wx + 1, wy + 25, ww - 2, wh - 26, FB_RGB(20, 20, 30));

    /* Draw terminal text */
    int start_row = term_cursor_y - (int)visible_rows + 1;
    if (start_row < 0) start_row = 0;

    for (uint32_t r = 0; r < visible_rows; r++) {
        int buf_row = start_row + (int)r;
        if (buf_row < 0 || buf_row >= TERM_ROWS) continue;
        for (uint32_t c = 0; c < visible_cols && c < TERM_COLS; c++) {
            char ch = term_buf[buf_row][c];
            if (ch) {
                font_draw_char(content_x + c * FONT_WIDTH,
                              content_y + r * FONT_HEIGHT,
                              ch, FB_RGB(0, 255, 100), FB_RGB(20, 20, 30));
            }
        }
    }

    /* Cursor blink */
    extern volatile uint64_t timer_ticks;
    if ((timer_ticks / 50) % 2 == 0) {
        int cr = term_cursor_y - start_row;
        if (cr >= 0 && cr < (int)visible_rows) {
            fb_fill_rect(content_x + term_cursor_x * FONT_WIDTH,
                        content_y + cr * FONT_HEIGHT,
                        FONT_WIDTH, FONT_HEIGHT,
                        FB_RGB(0, 255, 100));
        }
    }
}

/* ============================================================
 * GUI Main Loop
 * ============================================================ */

void gui_init(uint64_t fb_addr, uint32_t fb_width, uint32_t fb_height,
              uint32_t fb_pitch, uint32_t fb_bpp) {
    fb_init(fb_addr, fb_width, fb_height, fb_pitch, fb_bpp);
    mouse_init(fb_width, fb_height);

    /* Clear terminal buffer */
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            term_buf[r][c] = 0;
        }
    }

    gui_printf("SpiritFoxOS Terminal v1.0\n");
    gui_printf("Type 'help' for commands, 'exit' to close terminal.\n\n");
    gui_printf("root@SpiritFoxOS:~$ ");

    gui_running = 1;
}

void gui_run(void) {
    char input_buf[256];
    int input_pos = 0;

    while (gui_running) {
        /* Draw everything */
        draw_desktop();

        /* Desktop icons */
        font_draw_string(40, 50, "Terminal", FB_WHITE, FB_RGB(20, 30, 50));
        fb_draw_rect(30, 30, 64, 64, FB_SF_ACCENT);
        font_draw_string(38, 65, ">_", FB_WHITE, FB_SF_ACCENT);

        font_draw_string(130, 50, "System", FB_WHITE, FB_RGB(20, 30, 50));
        fb_draw_rect(120, 30, 64, 64, FB_SF_ORANGE);
        font_draw_string(132, 65, "SYS", FB_WHITE, FB_SF_ORANGE);

        font_draw_string(220, 50, "About", FB_WHITE, FB_RGB(20, 30, 50));
        fb_draw_rect(210, 30, 64, 64, FB_RGB(100, 50, 200));
        font_draw_string(218, 65, "i", FB_WHITE, FB_RGB(100, 50, 200));

        /* Terminal window */
        if (show_terminal) {
            uint32_t tw = 680;
            uint32_t th = 460;
            uint32_t tx = (fb.width - tw) / 2;
            uint32_t ty = 40;
            draw_window(tx, ty, tw, th, "Terminal - root@SpiritFoxOS", 1);
            draw_terminal_content(tx, ty, tw, th);
        }

        /* Draw mouse cursor */
        mouse_state_t ms = mouse_get_state();
        draw_cursor(ms.x, ms.y);

        /* Swap buffers */
        fb_swap_buffers();

        /* Process input */
        uint16_t key = keyboard_getkey();

        if (show_terminal) {
            if (!IS_SPECIAL_KEY(key)) {
                char c = (char)key;
                if (c == '\n') {
                    /* Execute command */
                    input_buf[input_pos] = '\0';
                    gui_printf("\n");

                    if (input_pos > 0) {
                        if (strcmp(input_buf, "exit") == 0 || strcmp(input_buf, "quit") == 0) {
                            show_terminal = 0;
                        } else if (strcmp(input_buf, "help") == 0) {
                            gui_printf("Available commands:\n");
                            gui_printf("  help    - Show this help\n");
                            gui_printf("  uname   - System info\n");
                            gui_printf("  uptime  - System uptime\n");
                            gui_printf("  mem     - Memory info\n");
                            gui_printf("  clear   - Clear terminal\n");
                            gui_printf("  neofetch- System info display\n");
                            gui_printf("  exit    - Close terminal\n");
                        } else if (strcmp(input_buf, "uname") == 0) {
                            gui_printf("SpiritFoxOS v1.0.0 x86_64\n");
                        } else if (strcmp(input_buf, "uptime") == 0) {
                            extern volatile uint64_t timer_ticks;
                            gui_printf("Uptime: %u seconds\n", (uint32_t)(timer_ticks / 100));
                        } else if (strcmp(input_buf, "mem") == 0) {
                            uint64_t free_p = pmm_free_count();
                            uint64_t total_p = pmm_total_count();
                            gui_printf("Memory: %u MB free / %u MB total\n",
                                       (uint32_t)(free_p * 4 / 1024),
                                       (uint32_t)(total_p * 4 / 1024));
                        } else if (strcmp(input_buf, "clear") == 0) {
                            for (int r = 0; r < TERM_ROWS; r++)
                                for (int c = 0; c < TERM_COLS; c++)
                                    term_buf[r][c] = 0;
                            term_cursor_x = 0;
                            term_cursor_y = 0;
                        } else if (strcmp(input_buf, "neofetch") == 0) {
                            gui_printf("  SpiritFoxOS v1.0.0 x86_64\n");
                            gui_printf("  Kernel: SpiritFoxOS-kernel\n");
                            gui_printf("  Shell:  sfsh 1.0\n");
                            gui_printf("  Resolution: %ux%u\n", fb.width, fb.height);
                            gui_printf("  Terminal: GUI Terminal\n");
                        } else {
                            gui_printf("sfsh: unknown command: %s\n", input_buf);
                        }
                    }
                    gui_printf("root@SpiritFoxOS:~$ ");
                    input_pos = 0;
                } else if (c == '\b') {
                    if (input_pos > 0) {
                        input_pos--;
                        if (term_cursor_x > 0) {
                            term_cursor_x--;
                            term_buf[term_cursor_y][term_cursor_x] = 0;
                        }
                    }
                } else if (c >= ' ' && input_pos < 255) {
                    input_buf[input_pos++] = c;
                    gui_printf("%c", c);
                }
            }
        } else {
            /* Desktop mode - handle mouse clicks */
            if (!IS_SPECIAL_KEY(key) && (char)key == '\n') {
                /* Open terminal on Enter */
                show_terminal = 1;
            }
        }

        /* Check mouse clicks for desktop icons */
        if (ms.buttons & 0x01) {
            /* Left click */
            if (!show_terminal) {
                /* Terminal icon */
                if (ms.x >= 30 && ms.x < 94 && ms.y >= 30 && ms.y < 94) {
                    show_terminal = 1;
                }
            } else {
                /* Close button check */
                uint32_t tw = 680;
                uint32_t tx = (fb.width - tw) / 2;
                if (ms.x >= (int32_t)(tx + tw - 22) && ms.x <= (int32_t)(tx + tw - 4) &&
                    ms.y >= 44 && ms.y <= 60) {
                    show_terminal = 0;
                }
            }
            /* Wait for button release */
            while (mouse_get_state().buttons & 0x01) {
                hlt();
            }
        }
    }
}
