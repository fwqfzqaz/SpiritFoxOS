#include "pic.h"
#include "../include/io.h"

void pic_init(void) {
    /* Start initialization in cascade mode */
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* Set vector offsets: PIC1 at IRQ32, PIC2 at IRQ40 */
    outb(PIC1_DATA, 32);
    io_wait();
    outb(PIC2_DATA, 40);
    io_wait();

    /* Tell PIC1 there's a slave at IRQ2 */
    outb(PIC1_DATA, 4);
    io_wait();
    /* Tell PIC2 its cascade identity */
    outb(PIC2_DATA, 2);
    io_wait();

    /* 8086 mode */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* Mask all IRQs except cascade (IRQ2) */
    outb(PIC1_DATA, 0xFB); /* Unmask IRQ2 (cascade) */
    outb(PIC2_DATA, 0xFF); /* Mask all */
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, 0x20);
    }
    outb(PIC1_COMMAND, 0x20);
}

void pic_mask_irq(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    uint8_t val = inb(port) | (1 << irq);
    outb(port, val);
}

void pic_unmask_irq(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    uint8_t val = inb(port) & ~(1 << irq);
    outb(port, val);
}
