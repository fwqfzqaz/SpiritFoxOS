/* SpiritFoxOS - 位图字体接口
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
#ifndef FONT_H
#define FONT_H

#include <stdint.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16
#define FONT_CHARS  128

void font_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);
void font_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t fg, uint32_t bg);
void font_draw_string_transparent(uint32_t x, uint32_t y, const char *str, uint32_t fg);

#endif /* FONT_H */
