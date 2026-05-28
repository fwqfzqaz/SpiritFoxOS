#include "serial.h"
#include "../include/io.h"

void serial_init(uint16_t port) {
    /* Disable all interrupts */
    outb(port + 1, 0x00);

    /* Enable DLAB (set baud rate divisor) */
    outb(port + 3, 0x80);

    /* Set divisor to 1 (115200 baud) */
    outb(port + 0, 0x01);
    outb(port + 1, 0x00);

    /* 8 bits, no parity, one stop bit */
    outb(port + 3, 0x03);

    /* Enable FIFO, clear them, 14-byte threshold */
    outb(port + 2, 0xC7);

    /* IRQs enabled, RTS/DSR set */
    outb(port + 4, 0x0B);
}

static int serial_transmit_empty(uint16_t port) {
    return inb(port + 5) & 0x20;
}

void serial_putchar(uint16_t port, char c) {
    while (!serial_transmit_empty(port));
    outb(port, c);
}

void serial_puts(uint16_t port, const char *str) {
    while (*str) {
        if (*str == '\n') serial_putchar(port, '\r');
        serial_putchar(port, *str++);
    }
}
