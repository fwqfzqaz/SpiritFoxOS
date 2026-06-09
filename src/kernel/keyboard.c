/* SpiritFoxOS - PS/2键盘驱动
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
#include "keyboard.h"
#include "idt.h"
#include "pic.h"
#include "vga.h"
#include "../include/io.h"

/* US keyboard scancode table (Set 1) */
static const char scancode_to_ascii[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

static const char scancode_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};

static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int alt_pressed = 0;

/* Ring buffer for key codes (16-bit to support special keys) */
#define KB_BUFFER_SIZE 256
static uint16_t kb_buffer[KB_BUFFER_SIZE];
static volatile uint32_t kb_buffer_head = 0;
static volatile uint32_t kb_buffer_tail = 0;

/* Extended scancode state */
static int extended_scancode = 0;

static void kb_push(uint16_t key) {
    uint32_t next = (kb_buffer_head + 1) % KB_BUFFER_SIZE;
    if (next != kb_buffer_tail) {
        kb_buffer[kb_buffer_head] = key;
        kb_buffer_head = next;
    }
}

void keyboard_init(void) {
    idt_register_handler(33, keyboard_handler);
    pic_unmask_irq(1);
}

void keyboard_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t scancode = inb(0x60);

    /* Extended key prefix (0xE0) */
    if (scancode == 0xE0) {
        extended_scancode = 1;
        goto done;
    }

    if (scancode & 0x80) {
        /* Key release */
        uint8_t key = scancode & 0x7F;
        if (extended_scancode) {
            extended_scancode = 0;
            goto done; /* Ignore extended key releases but still send EOI */
        }
        if (key == 0x2A || key == 0x36) shift_pressed = 0;
        if (key == 0x1D) ctrl_pressed = 0;
        if (key == 0x38) alt_pressed = 0;
    } else {
        /* Key press */
        if (extended_scancode) {
            extended_scancode = 0;
            /* Extended key press */
            switch (scancode) {
                case 0x48: kb_push(KEY_UP);       goto done;
                case 0x50: kb_push(KEY_DOWN);     goto done;
                case 0x4B: kb_push(KEY_LEFT);     goto done;
                case 0x4D: kb_push(KEY_RIGHT);    goto done;
                case 0x47: kb_push(KEY_HOME);     goto done;
                case 0x4F: kb_push(KEY_END);      goto done;
                case 0x49: kb_push(KEY_PAGEUP);   goto done;
                case 0x51: kb_push(KEY_PAGEDOWN); goto done;
                default:   goto done;
            }
        }

        /* Modifier keys */
        if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; goto done; }
        if (scancode == 0x1D) { ctrl_pressed = 1; goto done; }
        if (scancode == 0x38) { alt_pressed = 1; goto done; }

        /* Ctrl+C */
        if (ctrl_pressed && scancode == 0x2E) {
            kb_push(KEY_CTRL_C);
            goto done;
        }

        /* Tab */
        if (scancode == 0x0F) {
            kb_push(KEY_TAB);
            goto done;
        }

        /* Regular ASCII key */
        char c = shift_pressed ? scancode_shift[scancode] : scancode_to_ascii[scancode];
        if (c) {
            kb_push((uint16_t)c);
        }
    }

done:
    pic_send_eoi(1);
}

char keyboard_getchar(void) {
    while (kb_buffer_tail == kb_buffer_head) {
        hlt();
    }
    uint16_t key = kb_buffer[kb_buffer_tail];
    kb_buffer_tail = (kb_buffer_tail + 1) % KB_BUFFER_SIZE;
    /* If it's a special key, skip it for getchar */
    if (IS_SPECIAL_KEY(key)) {
        return keyboard_getchar(); /* Recurse to get next printable key */
    }
    return (char)key;
}

uint16_t keyboard_getkey(void) {
    while (kb_buffer_tail == kb_buffer_head) {
        hlt();
    }
    uint16_t key = kb_buffer[kb_buffer_tail];
    kb_buffer_tail = (kb_buffer_tail + 1) % KB_BUFFER_SIZE;
    return key;
}

int keyboard_has_key(void) {
    return kb_buffer_tail != kb_buffer_head;
}

uint16_t keyboard_try_getkey(void) {
    if (kb_buffer_tail == kb_buffer_head) return 0xFFFF;
    uint16_t key = kb_buffer[kb_buffer_tail];
    kb_buffer_tail = (kb_buffer_tail + 1) % KB_BUFFER_SIZE;
    return key;
}

/* ============================================================
 * USB HID Keyboard Support
 *
 * Converts HID boot protocol usage codes (0x04-0x39) to
 * our internal key codes, then pushes them into the same
 * ring buffer used by PS/2 keyboard.
 * ============================================================ */

/* HID Usage ID → ASCII mapping for printable keys.
 * Maps HID codes 0x04 ('a') through 0x39 ('~') to characters. */
static const char hid_to_ascii[] = {
    /* 0x00-0x03: reserved */
    0, 0, 0, 0,
    /* 0x04-0x27: a-z, 1-0 */
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
    'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',
    'u', 'v', 'w', 'x', 'y', 'z', '1', '2', '3', '4',
    '5', '6', '7', '8', '9', '0',
    /* 0x28-0x2D: Enter, Escape, Backspace, Tab, Space, -, = */
    '\n', 0x1B, '\b', '\t', ' ', '-', '=',
    /* 0x2E-0x37: [, ], \, #, ;, `, ,, . */
    '[', ']', '\\', '#', ';', '\'', ',', '.', '/',
    /* 0x38-0x39: CapsLock, PrintScreen etc - handled specially */
    0, 0,
};

/* HID Usage ID → special key code mapping */
static uint16_t hid_to_special(uint8_t hid_code) {
    switch (hid_code) {
        case 0x28: return '\n';           /* Enter */
        case 0x29: return KEY_ESCAPE;     /* Escape */
        case 0x2A: return '\b';           /* Backspace */
        case 0x2B: return KEY_TAB;        /* Tab */
        case 0x2C: return ' ';            /* Space */
        case 0x2D: return '-';            /* - */
        case 0x2E: return '=';            /* = */
        case 0x2F: return '[';            /* [ */
        case 0x30: return ']';            /* ] */
        case 0x31: return '\\';           /* \ */
        case 0x33: return ';';            /* ; */
        case 0x34: return '\'';           /* ' */
        case 0x35: return '`';            /* ` */
        case 0x36: return ',';            /* , */
        case 0x37: return '.';            /* . */
        case 0x38: return '/';            /* / */
        /* Modifier keys - we handle these via modifier byte separately */
        case 0xE0: return KEY_CTRL_C;     /* Ctrl (left) */
        case 0xE4: return KEY_TAB;        /* (not really tab but skip) */
        default:   return 0xFFFF;
    }
}

/* Called by usb_hid_poll_keyboard() with a HID usage code and modifier state */
void usb_kb_push(uint8_t hid_keycode, uint8_t modifiers) {
    (void)modifiers;  /* TODO: handle shift/ctrl/alt from USB modifiers */

    if (hid_keycode < 0x04 || hid_keycode > 0x65) return;

    /* Check for special keys first */
    if (hid_keycode >= 0x4A && hid_keycode <= 0x53) {
        /* Arrow keys, Home, End, Page Up/Down */
        static const uint16_t arrow_map[] = {
            KEY_RIGHT,      /* 0x4F Right */
            KEY_LEFT,       /* 0x50 Left */
            KEY_DOWN,       /* 0x51 Down */
            KEY_UP,         /* 0x52 Up */
            0xFFFF,         /* 0x53 NumLock? */
        };
        if (hid_keycode >= 0x4F && hid_keycode <= 0x52) {
            kb_push(arrow_map[hid_keycode - 0x4F]);
        }
        return;
    }

    if (hid_keycode >= 0x3A && hid_keycode <= 0x45) {
        /* F1-F12 range starts at 0x3A */
        if (hid_keycode >= 0x3A && hid_keycode <= 0x45) {
            kb_push(KEY_F1 + (hid_keycode - 0x3A));
        }
        return;
    }

    /* Try special key mapping */
    uint16_t special = hid_to_special(hid_keycode);
    if (special != 0xFFFF) {
        kb_push(special);
        return;
    }

    /* Printable character from HID code */
    if (hid_keycode < sizeof(hid_to_ascii)) {
        char c = hid_to_ascii[hid_keycode];
        if (c != 0) {
            kb_push((uint16_t)c);
        }
    }
}
