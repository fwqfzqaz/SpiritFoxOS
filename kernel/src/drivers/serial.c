#include "serial.h"
#include "hal.h"

#define COM1 0x3F8

void serial_init(void)
{
    hal_outb(COM1 + 1, 0x00);    // 禁用中断
    hal_outb(COM1 + 3, 0x80);    // 使能 DLAB
    hal_outb(COM1 + 0, 0x03);    // 波特率除数低位 (115200)
    hal_outb(COM1 + 1, 0x00);    // 波特率除数高位
    hal_outb(COM1 + 3, 0x03);    // 8 位数据位，无校验，1 位停止位
    hal_outb(COM1 + 2, 0xC7);    // 使能 FIFO
    hal_outb(COM1 + 4, 0x0B);    // 使能 IRQ，RTS/DSR 已设置
}

void serial_putchar(char c)
{
    while (!(hal_inb(COM1 + 5) & 0x20));
    hal_outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s) {
        serial_putchar(*s);
        s++;
    }
}

void serial_put_hex(uint64_t val)
{
    static const char hex[] = "0123456789abcdef";
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        serial_putchar(hex[(val >> i) & 0xF]);
    }
}

void serial_put_dec(uint64_t val)
{
    char buf[21];
    int pos = 0;
    if (val == 0) {
        serial_putchar('0');
        return;
    }
    while (val > 0) {
        buf[pos++] = '0' + (val % 10);
        val /= 10;
    }
    for (int i = pos - 1; i >= 0; i--) {
        serial_putchar(buf[i]);
    }
}
