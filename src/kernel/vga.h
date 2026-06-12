/* SpiritFoxOS - VGA文本模式接口
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
#ifndef VGA_H
#define VGA_H

#include <stdint.h>

#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_BUFFER ((volatile uint16_t *)0xB8000)

/* VGA颜色 */
typedef enum {
    VGA_BLACK       = 0,
    VGA_BLUE        = 1,
    VGA_GREEN       = 2,
    VGA_CYAN        = 3,
    VGA_RED         = 4,
    VGA_MAGENTA     = 5,
    VGA_BROWN       = 6,
    VGA_LIGHT_GREY  = 7,
    VGA_DARK_GREY   = 8,
    VGA_LIGHT_BLUE  = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN  = 11,
    VGA_LIGHT_RED   = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW      = 14,
    VGA_WHITE       = 15
} vga_color_t;

void vga_init(void);
void vga_putchar(char c);
void vga_puts(const char *str);
void vga_set_color(vga_color_t fg, vga_color_t bg);
void vga_clear(void);
void vga_set_cursor(uint8_t col, uint8_t row);
void vga_get_cursor(uint8_t *col, uint8_t *row);
void vga_printf(const char *fmt, ...);

#endif /* VGA_H */
