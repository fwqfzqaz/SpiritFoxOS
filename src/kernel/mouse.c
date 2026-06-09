/* SpiritFoxOS - PS/2鼠标驱动
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
#include "mouse.h"
#include "pic.h"
#include "../include/io.h"

static mouse_state_t state;
static uint8_t mouse_cycle = 0;
static uint8_t mouse_bytes[4];

/* ---- Low-level PS/2 helpers ---- */

static void ps2_wait_write(void) {
    uint32_t t = 100000;
    while (--t && (inb(0x64) & 0x02))
        ;
}

static void ps2_wait_read(void) {
    uint32_t t = 100000;
    while (--t && !(inb(0x64) & 0x01))
        ;
}

/* Discard any data sitting in the output buffer */
static inline void ps2_drain(void) {
    while (inb(0x64) & 0x01)
        inb(0x60);
}

static inline void ps2_delay(void) {
    for (volatile int i = 0; i < 50000; i++)
        ;
}

/* Send a byte to the auxiliary device (mouse), read response.
 * Returns response byte or 0xFF on timeout. */
static uint8_t aux_cmd(uint8_t cmd) {
    ps2_wait_write();
    outb(0x64, 0xD4);          /* Write to aux device */
    ps2_wait_write();
    outb(0x60, cmd);            /* Command byte */
    ps2_wait_read();             /* Wait for response */
    return inb(0x60);            /* Read ACK/response */
}

/* Send command with up to 3 retries on RESEND (0xFE). */
static int aux_cmd_ok(uint8_t cmd) {
    for (int i = 0; i < 3; i++) {
        uint8_t r = aux_cmd(cmd);
        if (r == 0xFA) return 1;   /* ACK */
        if (r == 0xFE) continue;   /* RESEND */
        break;                     /* Unexpected */
    }
    return 0;
}

/* ---- Initialization ---- */

void mouse_init(uint32_t screen_w, uint32_t screen_h) {
    state.x = (int32_t)(screen_w / 2);
    state.y = (int32_t)(screen_h / 2);
    state.buttons = 0;
    state.scroll = 0;
    state.screen_width = screen_w;
    state.screen_height = screen_h;
    mouse_cycle = 0;

    cli();

    /* Drain any stale bytes from output buffer */
    ps2_drain();
    ps2_delay();

    /* Enable PS/2 auxiliary device (mouse port).
     * This does NOT reset the controller or affect keyboard port. */
    ps2_wait_write();
    outb(0x64, 0xA8);
    ps2_delay();

    /* Read current config, enable IRQ12 (bit 1), keep everything else.
     * Do NOT touch bit 6 (translation) — keyboard needs it! */
    ps2_wait_write();
    outb(0x64, 0x20);            /* Read config */
    ps2_wait_read();
    uint8_t cfg = inb(0x60);

    if (!(cfg & 0x02)) {          /* Only modify if IRQ12 not already set */
        cfg |= 0x02;              /* Enable mouse interrupt (IRQ12) */
        ps2_wait_write();
        outb(0x64, 0x60);         /* Write config */
        ps2_wait_write();
        outb(0x60, cfg);
    }

    ps2_delay();
    ps2_drain();

    /* Reset mouse: sends ACK + BAT OK(0xAA) + Device ID(0x00) */
    aux_cmd(0xFF);
    ps2_delay();

    /* Consume BAT result and Device ID (may not always appear) */
    ps2_wait_read();
    inb(0x60);                    /* BAT result (should be 0xAA) */
    ps2_wait_read();
    inb(0x60);                    /* Device ID (usually 0x00) */

    ps2_drain();
    ps2_delay();

    /* Set defaults: clears scaling/resolution overrides */
    aux_cmd_ok(0xF6);
    ps2_delay();
    ps2_drain();

    /* Enable data reporting — mouse starts sending movement packets */
    aux_cmd_ok(0xF4);
    ps2_delay();
    ps2_drain();

    sti();

    /* Register interrupt handler and unmask IRQ12 */
    idt_register_handler(44, mouse_handler);
    pic_unmask_irq(12);
}

/* ---- Interrupt Handler ---- */

void mouse_handler(struct interrupt_frame *frame) {
    (void)frame;

    /*
     * On real hardware, IRQ12 fires for both mouse AND sometimes keyboard data.
     * Bit 5 of status register (0x64) tells us if data is from the
     * auxiliary device (mouse) or from the keyboard port.
     *
     * QEMU usually doesn't need this check because it correctly routes
     * interrupts, but real PS/2 controllers often require it.
     */
    uint8_t status = inb(0x64);
    if (!(status & 0x20)) {
        /* Data came from keyboard, not mouse — discard it */
        (void)inb(0x60);
        pic_send_eoi(12);
        return;
    }

    uint8_t data = inb(0x60);
    mouse_bytes[mouse_cycle] = data;

    if (mouse_cycle == 0) {
        /* First byte of 3-byte packet: bits [3:0] must have bit 3 set */
        if (!(data & 0x08)) {
            /*
             * Out of sync — discard this byte and reset the cycle counter.
             *
             * IMPORTANT: We must reset mouse_cycle to 0 (not leave it or increment).
             * If we don't reset, a dropped byte could cause permanent desync where
             * byte 2 is treated as byte 0, byte 0 as byte 1, etc., causing Y
             * displacement to be misinterpreted as button state (the root cause
             * of "button triggers during movement" bug).
             */
            mouse_cycle = 0;  /* Stay at 0, keep looking for valid packet start */
            pic_send_eoi(12);
            return;
        }
        mouse_cycle++;
    } else if (mouse_cycle == 1) {
        /* Second byte: X displacement */
        mouse_cycle++;
    } else {
        /* Third byte: Y displacement — full packet received */
        mouse_cycle = 0;

        /*
         * PS/2 Mouse Packet Format (3 bytes):
         *   Byte 0: [Yovf][Xovf][Ysgn][Xsgn][1][Mid][Rgt][Lft]
         *   Byte 1: X displacement (signed, sign-extended from byte 0 bits 4-5)
         *   Byte 2: Y displacement (signed, sign-extended from byte 0 bits 6-7)
         *
         * Button state is in BYTE 0 low 3 bits, NOT in byte 2.
         * Reading buttons from byte 2 (Y displacement) causes spurious
         * button events during every mouse movement — the root cause of
         * "auto-click on move" bug.
         */
        uint8_t new_buttons = mouse_bytes[0] & 0x07;
        if (new_buttons != state.buttons) {
            /*
             * Additional sanity check: if both X and Y displacement are zero
             * but buttons changed, it's likely a genuine button event.
             * If there IS movement and buttons changed, verify the change is
             * plausible (not all bits flipping wildly).
             */
            int8_t dx_raw = (int8_t)mouse_bytes[1];
            int8_t dy_raw = (int8_t)mouse_bytes[2];

            uint8_t changed = new_buttons ^ state.buttons;
            if (dx_raw != 0 || dy_raw != 0) {
                /*
                 * Movement + button change: use conservative filtering.
                 * Only allow single-bit changes during movement to filter out
                 * corruption where multiple bits flip simultaneously (which
                 * is physically impossible for a real mouse — you can't press
                 * and release multiple buttons in the same sample period).
                 */
                if ((changed & (changed - 1)) == 0) {
                    /* Single bit changed — likely legitimate */
                    state.buttons = new_buttons;
                }
                /* Else: multiple bits changed during movement → ignore (noise) */
            } else {
                /* No movement, only buttons changed → accept (genuine event) */
                state.buttons = new_buttons;
            }
        }
        /* If new_buttons == state.buttons, no update needed (optimization) */

        int32_t dx = (int32_t)(int8_t)mouse_bytes[1];
        int32_t dy = (int32_t)(int8_t)mouse_bytes[2];
        dy = -dy;                /* PS/2 Y-axis is inverted vs screen */

        state.x += dx;
        state.y += dy;

        /* Clamp to screen bounds */
        if (state.x < 0) state.x = 0;
        if (state.y < 0) state.y = 0;
        if (state.x >= (int32_t)state.screen_width)
            state.x = (int32_t)state.screen_width - 1;
        if (state.y >= (int32_t)state.screen_height)
            state.y = (int32_t)state.screen_height - 1;
    }

    pic_send_eoi(12);
}

/* ---- Public API ---- */

mouse_state_t mouse_get_state(void) {
    return state;
}

/* Called by USB HID mouse driver to merge USB mouse input into shared state */
void usb_mouse_update(int8_t dx, int8_t dy, uint8_t buttons) {
    state.x += dx;
    state.y += dy;

    /* Merge button states: bit0=left, bit1=right, bit2=middle */
    if (buttons & 0x01) state.buttons |= 0x01; else state.buttons &= ~0x01;
    if (buttons & 0x02) state.buttons |= 0x02; else state.buttons &= ~0x02;
    if (buttons & 0x04) state.buttons |= 0x04; else state.buttons &= ~0x04;

    if (state.x < 0) state.x = 0;
    if (state.y < 0) state.y = 0;
    if (state.x >= (int32_t)state.screen_width)
        state.x = (int32_t)state.screen_width - 1;
    if (state.y >= (int32_t)state.screen_height)
        state.y = (int32_t)state.screen_height - 1;
}
