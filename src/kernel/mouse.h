/* SpiritFoxOS - PS/2鼠标接口
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
#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include "idt.h"

typedef struct {
    int32_t x, y;
    uint8_t buttons;       /* bit0=left, bit1=right, bit2=middle */
    int8_t scroll;
    uint32_t screen_width;
    uint32_t screen_height;
} mouse_state_t;

void mouse_init(uint32_t screen_w, uint32_t screen_h);
void mouse_handler(struct interrupt_frame *frame);
mouse_state_t mouse_get_state(void);

/* USB HID mouse integration */
void usb_mouse_update(int8_t dx, int8_t dy, uint8_t buttons);

#endif /* MOUSE_H */
