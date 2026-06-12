/* SpiritFoxOS - 可编程中断控制器驱动
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
#include "pic.h"
#include "../include/io.h"

void pic_init(void) {
    /* 以级联模式开始初始化 */
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* 设置向量偏移：PIC1在IRQ32，PIC2在IRQ40 */
    outb(PIC1_DATA, 32);
    io_wait();
    outb(PIC2_DATA, 40);
    io_wait();

    /* 告诉PIC1从片连接在IRQ2上 */
    outb(PIC1_DATA, 4);
    io_wait();
    /* 告诉PIC2其级联标识 */
    outb(PIC2_DATA, 2);
    io_wait();

    /* 8086模式 */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* 屏蔽除级联（IRQ2）外的所有IRQ */
    outb(PIC1_DATA, 0xFB); /* 解除IRQ2屏蔽（级联） */
    outb(PIC2_DATA, 0xFF); /* 屏蔽全部 */
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
