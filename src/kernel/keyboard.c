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
            return; /* Ignore extended key releases */
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
