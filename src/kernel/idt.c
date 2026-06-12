/* SpiritFoxOS - 中断描述符表
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
#include "idt.h"
#include "pic.h"
#include "../include/io.h"

static struct idt_entry idt[IDT_ENTRIES];
static struct idtr idt_ptr;
static interrupt_handler_t handlers[IDT_ENTRIES] = {0};

void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t type_attr) {
    idt[num].offset_low  = handler & 0xFFFF;
    idt[num].selector    = selector;
    idt[num].ist         = 0;
    idt[num].type_attr   = type_attr;
    idt[num].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].reserved    = 0;
}

void idt_register_handler(uint8_t num, interrupt_handler_t handler) {
    handlers[num] = handler;
}

/* 从汇编调用的通用中断处理函数 */
void interrupt_handler(struct interrupt_frame *frame) {
    if (handlers[frame->int_no]) {
        handlers[frame->int_no](frame);
    } else if (frame->int_no >= 32 && frame->int_no < 48) {
        pic_send_eoi(frame->int_no - 32);
    }
}

void idt_init(void) {
    /* 清零IDT */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt[i].offset_low  = 0;
        idt[i].selector    = 0;
        idt[i].ist         = 0;
        idt[i].type_attr   = 0;
        idt[i].offset_mid  = 0;
        idt[i].offset_high = 0;
        idt[i].reserved    = 0;
    }

    /* 设置ISR门（0-31: CPU异常，32-255: IRQ/系统调用） */
    for (int i = 0; i < 48; i++) {
        idt_set_gate(i, isr_table[i], 0x08, 0x8E); /* 存在、DPL=0、中断门 */
    }

    /* 加载IDTR */
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint64_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));
}
