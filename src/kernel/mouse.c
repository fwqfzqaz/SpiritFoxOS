#include "mouse.h"
#include "pic.h"
#include "../include/io.h"

static mouse_state_t state;
static uint8_t mouse_cycle = 0;
static uint8_t mouse_bytes[4];

static void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) {
        while (timeout--) { if ((inb(0x64) & 1) == 1) return; }
    } else {
        while (timeout--) { if ((inb(0x64) & 2) == 0) return; }
    }
}

static void mouse_write(uint8_t val) {
    mouse_wait(1);
    outb(0x64, 0xD4);
    mouse_wait(1);
    outb(0x60, val);
}

static uint8_t mouse_read(void) {
    mouse_wait(0);
    return inb(0x60);
}

void mouse_init(uint32_t screen_w, uint32_t screen_h) {
    state.x = (int32_t)(screen_w / 2);
    state.y = (int32_t)(screen_h / 2);
    state.buttons = 0;
    state.scroll = 0;
    state.screen_width = screen_w;
    state.screen_height = screen_h;

    uint8_t status;

    /* Enable auxiliary device (mouse) */
    mouse_wait(1);
    outb(0x64, 0xA8);

    /* Get controller command byte */
    mouse_wait(1);
    outb(0x64, 0x20);
    mouse_wait(0);
    status = (inb(0x60) | 0x02) & ~0x20;

    /* Set controller command byte */
    mouse_wait(1);
    outb(0x64, 0x60);
    mouse_wait(1);
    outb(0x60, status);

    /* Set defaults */
    mouse_write(0xF6);
    mouse_read(); /* ACK */

    /* Enable data reporting */
    mouse_write(0xF4);
    mouse_read(); /* ACK */

    /* Register IRQ12 handler (IRQ12 = 32 + 12 = 44) */
    idt_register_handler(44, mouse_handler);
    pic_unmask_irq(12);
}

void mouse_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t data = inb(0x60);

    mouse_bytes[mouse_cycle] = data;
    mouse_cycle++;

    if (mouse_cycle >= 3) {
        mouse_cycle = 0;

        /* Sync check: bit 3 of byte 0 must be set */
        if (!(mouse_bytes[0] & 0x08)) {
            pic_send_eoi(12);
            return;
        }

        /* Parse buttons */
        state.buttons = mouse_bytes[0] & 0x07;

        /* Parse X delta */
        int32_t dx = (int32_t)(int8_t)mouse_bytes[1];
        if (mouse_bytes[0] & 0x10) dx |= 0xFFFFFF00;
        else dx &= 0xFF;

        /* Parse Y delta (PS/2 Y is inverted) */
        int32_t dy = -(int32_t)(int8_t)mouse_bytes[2];

        /* Update position */
        state.x += dx;
        state.y += dy;

        /* Clamp to screen bounds */
        if (state.x < 0) state.x = 0;
        if (state.y < 0) state.y = 0;
        if (state.x >= (int32_t)state.screen_width) state.x = (int32_t)state.screen_width - 1;
        if (state.y >= (int32_t)state.screen_height) state.y = (int32_t)state.screen_height - 1;
    }

    pic_send_eoi(12);
}

mouse_state_t mouse_get_state(void) {
    return state;
}
