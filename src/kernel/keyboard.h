/* SpiritFoxOS - PS/2键盘接口
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
#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include "idt.h"

/* Special key codes (returned by keyboard_getkey) */
#define KEY_ESCAPE    0x100
#define KEY_UP        0x101
#define KEY_DOWN      0x102
#define KEY_LEFT      0x103
#define KEY_RIGHT     0x104
#define KEY_HOME      0x105
#define KEY_END       0x106
#define KEY_PAGEUP    0x107
#define KEY_PAGEDOWN  0x108
#define KEY_F1        0x10A
#define KEY_F2        0x10B
#define KEY_F3        0x10C
#define KEY_F4        0x10D
#define KEY_F5        0x10E
#define KEY_F6        0x10F
#define KEY_F7        0x110
#define KEY_F8        0x111
#define KEY_F9        0x112
#define KEY_F10       0x113
#define KEY_F11       0x114
#define KEY_F12       0x115
#define KEY_TAB       0x116
#define KEY_CTRL_C    0x117

/* Check if a key code is a special (non-printable) key */
#define IS_SPECIAL_KEY(k) ((k) >= 0x100)

void keyboard_init(void);
void keyboard_handler(struct interrupt_frame *frame);
char keyboard_getchar(void);
uint16_t keyboard_getkey(void);
int keyboard_has_key(void);
uint16_t keyboard_try_getkey(void);

/* USB HID keyboard integration */
void usb_kb_push(uint8_t hid_keycode, uint8_t modifiers);

#endif /* KEYBOARD_H */
