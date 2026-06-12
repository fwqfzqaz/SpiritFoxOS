/* SpiritFoxOS - 串口驱动
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
#include "serial.h"
#include "../include/io.h"

void serial_init(uint16_t port) {
    /* 禁用所有中断 */
    outb(port + 1, 0x00);

    /* 启用DLAB（设置波特率分频值） */
    outb(port + 3, 0x80);

    /* 设置分频值为1（115200波特率） */
    outb(port + 0, 0x01);
    outb(port + 1, 0x00);

    /* 8位数据位，无校验，1位停止位 */
    outb(port + 3, 0x03);

    /* 启用FIFO，清除FIFO，14字节阈值 */
    outb(port + 2, 0xC7);

    /* 启用IRQ，设置RTS/DSR */
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

int serial_has_data(uint16_t port) {
    return inb(port + 5) & 0x01;
}

char serial_getchar(uint16_t port) {
    while (!serial_has_data(port));
    return inb(port);
}
