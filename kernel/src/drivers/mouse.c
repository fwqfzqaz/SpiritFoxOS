/*
 * mouse.c - PS/2 mouse driver for SpiritFoxOS
 *
 * Uses the auxiliary device port of the 8042 keyboard controller.
 * IRQ12 (vector 44) fires once per byte; 3-byte packets are decoded.
 *
 * Key design notes:
 *   - Port 0x60 is shared between keyboard and mouse. We check the
 *     KBC status register bit 5 to confirm data came from the mouse.
 *   - Packet synchronization is maintained by always looking for a
 *     valid byte-0 (bit 3 set, bits 6-7 clear) before accepting bytes.
 */

#include "mouse.h"
#include "hal.h"
#include "vga.h"

/* ---- 8042 keyboard controller ports ---- */
#define KBC_CMD    0x64
#define KBC_DATA   0x60
#define KBC_STATUS 0x64

/* KBC status register bits */
#define KBC_STS_OBF   0x01   /* Output buffer full */
#define KBC_STS_AUX   0x20   /* Data from auxiliary (mouse) device */

/* ---- Internal state ---- */
static int      mouse_x;
static int      mouse_y;
static uint8_t  mouse_buttons;

static volatile int mouse_event_pending;

/* ---- 3-byte packet reassembly ---- */
static uint8_t  pkt_buf[3];
static int      pkt_idx;

/* ---- Helpers ---- */

static void kbc_wait_write(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(hal_inb(KBC_STATUS) & 0x02))
            return;
    }
}

static void kbc_wait_read(void)
{
    for (int i = 0; i < 100000; i++) {
        if (hal_inb(KBC_STATUS) & KBC_STS_OBF)
            return;
    }
}

/* Check if the next byte in the output buffer is from the mouse */
static int kbc_is_aux_data(void)
{
    return (hal_inb(KBC_STATUS) & KBC_STS_AUX) != 0;
}

/* Send a command to the mouse via the KBC aux port */
static int mouse_cmd(uint8_t cmd)
{
    uint8_t ack;

    for (int retry = 0; retry < 3; retry++) {
        kbc_wait_write();
        hal_outb(KBC_CMD, 0xD4);         /* route next byte to mouse */
        kbc_wait_write();
        hal_outb(KBC_DATA, cmd);

        /* Wait for ACK (0xFA) from the mouse */
        for (int i = 0; i < 100000; i++) {
            uint8_t sts = hal_inb(KBC_STATUS);
            if (sts & KBC_STS_OBF) {
                if (sts & KBC_STS_AUX) {
                    ack = hal_inb(KBC_DATA);
                    if (ack == 0xFA)
                        return 0;        /* success */
                    if (ack == 0xFE)
                        break;           /* resend requested, retry */
                } else {
                    /* Keyboard data arrived - discard it */
                    hal_inb(KBC_DATA);
                }
            }
        }
    }
    return -1;  /* failed after retries */
}

/* ---- Check if byte looks like a valid first byte of a PS/2 packet ---- */
static int is_valid_first_byte(uint8_t b)
{
    /* Bit 3 must be set (always 1 in valid packets) */
    if (!(b & 0x08))
        return 0;
    /* Bits 6-7 should be 0 (reserved, always 0 in standard PS/2) */
    if (b & 0xC0)
        return 0;
    return 1;
}

/* ---- Public: initialize the PS/2 mouse ---- */

void mouse_init(void)
{
    pkt_idx = 0;
    mouse_x = 512;
    mouse_y = 384;
    mouse_buttons = 0;
    mouse_event_pending = 0;

    /* 1. Disable devices during setup to avoid spurious data */
    kbc_wait_write();
    hal_outb(KBC_CMD, 0xA7);          /* disable aux (mouse) port */
    kbc_wait_write();
    hal_outb(KBC_CMD, 0xAD);          /* disable keyboard port */

    /* 2. Flush any pending data in the output buffer */
    while (hal_inb(KBC_STATUS) & KBC_STS_OBF)
        hal_inb(KBC_DATA);

    /* 3. Enable auxiliary device (mouse port) */
    kbc_wait_write();
    hal_outb(KBC_CMD, 0xA8);          /* enable aux port */

    /* 4. Configure KBC command byte: enable both IRQs, enable keyboard */
    kbc_wait_write();
    hal_outb(KBC_CMD, 0x60);
    kbc_wait_write();
    hal_outb(KBC_DATA, 0x47);         /* bit0=KB_INT, bit1=M_INT, bit6=scan_conv */

    /* 5. Reset mouse */
    if (mouse_cmd(0xFF) != 0) {
        printf("[MOUSE] Reset failed, retrying...\n");
        mouse_cmd(0xFF);
    }

    /* Read mouse self-test result (0xAA) and mouse ID (0x00) */
    kbc_wait_read();
    if (kbc_is_aux_data())
        hal_inb(KBC_DATA);            /* 0xAA = self-test OK */
    kbc_wait_read();
    if (kbc_is_aux_data())
        hal_inb(KBC_DATA);            /* 0x00 = standard mouse ID */

    /* 6. Set sample rate (100 samples/sec - default) */
    mouse_cmd(0xF3);                  /* set sample rate */
    mouse_cmd(100);                   /* 100 Hz */

    /* 7. Set resolution (4 counts/mm - default) */
    mouse_cmd(0xE8);                  /* set resolution */
    mouse_cmd(0x02);                  /* 4 counts/mm */

    /* 8. Set scaling 1:1 */
    mouse_cmd(0xE6);

    /* 9. Enable data reporting (stream mode) */
    mouse_cmd(0xF4);

    /* 10. Re-enable keyboard port */
    kbc_wait_write();
    hal_outb(KBC_CMD, 0xAE);          /* enable keyboard port */

    printf("[MOUSE] PS/2 mouse initialized\n");
}

/* ---- Public: IRQ12 handler, called from irq_handler ---- */

void mouse_handler(void)
{
    /* Verify this data is from the mouse (not keyboard) */
    uint8_t sts = hal_inb(KBC_STATUS);
    if (!(sts & KBC_STS_OBF))
        return;                        /* spurious interrupt */
    if (!(sts & KBC_STS_AUX)) {
        /* Data is from keyboard, not mouse - let keyboard handler deal with it */
        /* Read and discard to clear the interrupt, but this shouldn't normally happen */
        hal_inb(KBC_DATA);
        return;
    }

    uint8_t data = hal_inb(KBC_DATA);

    /* Packet synchronization:
     * If we're expecting byte 0, check if this byte looks like a valid
     * first byte. If not, keep discarding until we find one.
     * If we're at byte 1 or 2 and get something that looks like byte 0,
     * it means we lost a byte - resync. */
    if (pkt_idx == 0) {
        if (!is_valid_first_byte(data)) {
            /* Not a valid first byte - skip it and stay at index 0 */
            return;
        }
    } else {
        /* We're at byte 1 or 2 - but if this looks like a new first byte,
         * our previous packet was incomplete. Restart. */
        if (is_valid_first_byte(data)) {
            pkt_buf[0] = data;
            pkt_idx = 1;
            return;
        }
    }

    pkt_buf[pkt_idx++] = data;

    if (pkt_idx < 3)
        return;

    /* Complete packet received */
    pkt_idx = 0;

    /* Decode button state */
    mouse_buttons = pkt_buf[0] & 0x07;

    /* Decode X/Y movement (9-bit signed):
     *   Low 8 bits in byte 1/2, sign bit in byte 0 bit 4/5.
     *   Must NOT use (int8_t) cast — that uses bit 7 of the movement
     *   byte as the sign, which is wrong.  The sign comes from byte 0. */
    int dx = (int)pkt_buf[1];
    int dy = (int)pkt_buf[2];
    if (pkt_buf[0] & 0x10) dx -= 256;  /* negative X */
    if (pkt_buf[0] & 0x20) dy -= 256;  /* negative Y */
    dy = -dy;   /* PS/2 Y axis is inverted (up = negative) */

    /* Speed multiplier for comfortable cursor movement */
    dx *= 2;
    dy *= 2;

    /* Update absolute position */
    mouse_x += dx;
    mouse_y += dy;

    /* Clamp to screen bounds */
    if (mouse_x < 0)   mouse_x = 0;
    if (mouse_x > 1023) mouse_x = 1023;
    if (mouse_y < 0)   mouse_y = 0;
    if (mouse_y > 767) mouse_y = 767;

    mouse_event_pending = 1;
}

/* ---- Public: polling interface ---- */

int mouse_has_event(void)
{
    return mouse_event_pending;
}

void mouse_get_state(int *x, int *y, uint8_t *buttons)
{
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (buttons) *buttons = mouse_buttons;
    mouse_event_pending = 0;
}

void mouse_get_delta(int *dx, int *dy, uint8_t *buttons)
{
    if (dx) *dx = mouse_x;
    if (dy) *dy = mouse_y;
    if (buttons) *buttons = mouse_buttons;
    mouse_event_pending = 0;
}
