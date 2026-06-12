/* SpiritFoxOS - VGA文本模式驱动
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
#include "vga.h"
#include "../include/io.h"
#include <stdarg.h>

static uint8_t cursor_x = 0;
static uint8_t cursor_y = 0;
static uint8_t current_color = 0x07; /* 浅灰色黑底 */

/* 前向声明 */
void vga_update_hw_cursor(void);

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

void vga_init(void) {
    vga_clear();
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

void vga_set_color(vga_color_t fg, vga_color_t bg) {
    current_color = (uint8_t)(fg | (bg << 4));
}

void vga_clear(void) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        VGA_BUFFER[i] = vga_entry(' ', current_color);
    }
    cursor_x = 0;
    cursor_y = 0;
    vga_update_hw_cursor();
}

static void vga_scroll(void) {
    /* 将所有行上移一行 */
    for (int y = 0; y < VGA_ROWS - 1; y++) {
        for (int x = 0; x < VGA_COLS; x++) {
            VGA_BUFFER[y * VGA_COLS + x] = VGA_BUFFER[(y + 1) * VGA_COLS + x];
        }
    }
    /* 清除最后一行 */
    for (int x = 0; x < VGA_COLS; x++) {
        VGA_BUFFER[(VGA_ROWS - 1) * VGA_COLS + x] = vga_entry(' ', current_color);
    }
    cursor_y = VGA_ROWS - 1;
}

void vga_update_hw_cursor(void) {
    uint16_t pos = cursor_y * VGA_COLS + cursor_x;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_putchar(char c) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        cursor_x = (cursor_x + 8) & ~7;
        if (cursor_x >= VGA_COLS) {
            cursor_x = 0;
            cursor_y++;
        }
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            VGA_BUFFER[cursor_y * VGA_COLS + cursor_x] = vga_entry(' ', current_color);
        }
    } else {
        VGA_BUFFER[cursor_y * VGA_COLS + cursor_x] = vga_entry(c, current_color);
        cursor_x++;
        if (cursor_x >= VGA_COLS) {
            cursor_x = 0;
            cursor_y++;
        }
    }

    if (cursor_y >= VGA_ROWS) {
        vga_scroll();
    }
    vga_update_hw_cursor();
}

void vga_puts(const char *str) {
    while (*str) {
        vga_putchar(*str++);
    }
}

void vga_set_cursor(uint8_t col, uint8_t row) {
    cursor_x = col;
    cursor_y = row;
    vga_update_hw_cursor();
}

void vga_get_cursor(uint8_t *col, uint8_t *row) {
    *col = cursor_x;
    *row = cursor_y;
}

/* 简单的printf实现 */
static void vga_print_dec(int64_t val) {
    if (val < 0) {
        vga_putchar('-');
        val = -val;
    }
    if (val == 0) {
        vga_putchar('0');
        return;
    }
    char buf[20];
    int i = 0;
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (--i >= 0) {
        vga_putchar(buf[i]);
    }
}

static void vga_print_hex(uint64_t val, int width) {
    const char hex[] = "0123456789ABCDEF";
    char buf[17];
    buf[16] = '\0';
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    /* 跳过前导零但保留至少'width'位数字 */
    int start = 16 - width;
    if (start < 0) start = 0;
    while (start < 15 && buf[start] == '0' && (16 - start) > width) {
        start++;
    }
    vga_puts(&buf[start]);
}

void vga_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 'd':
                case 'i':
                    vga_print_dec(va_arg(args, int64_t));
                    break;
                case 'u':
                    vga_print_dec(va_arg(args, uint64_t));
                    break;
                case 'x':
                    vga_print_hex(va_arg(args, uint64_t), 1);
                    break;
                case 'p':
                    vga_puts("0x");
                    vga_print_hex(va_arg(args, uint64_t), 16);
                    break;
                case 'c':
                    vga_putchar((char)va_arg(args, int));
                    break;
                case 's':
                    vga_puts(va_arg(args, const char *));
                    break;
                case '%':
                    vga_putchar('%');
                    break;
                default:
                    vga_putchar('%');
                    vga_putchar(*fmt);
                    break;
            }
        } else {
            vga_putchar(*fmt);
        }
        fmt++;
    }

    va_end(args);
}
