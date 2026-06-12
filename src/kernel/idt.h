/* SpiritFoxOS - 中断描述符表接口
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
#ifndef IDT_H
#define IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

/* IDT表项（64位模式下16字节） */
struct idt_entry {
    uint16_t offset_low;     /* 偏移量 [15:0] */
    uint16_t selector;       /* 代码段选择子 */
    uint8_t  ist;            /* IST偏移（0 = 不使用IST） */
    uint8_t  type_attr;      /* 类型和属性 */
    uint16_t offset_mid;     /* 偏移量 [31:16] */
    uint32_t offset_high;    /* 偏移量 [63:32] */
    uint32_t reserved;       /* 必须为零 */
} __attribute__((packed));

/* IDTR寄存器 */
struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* 中断帧（由CPU和我们的处理程序压入栈）
 * 栈布局从低地址到高地址（RSP指向此处）：
 *   ds          <- 最后由isr_common压入（用ds值push rax）
 *   r15-r8      <- 按顺序压入：r15最后，r8最先
 *   rdi,rsi,rbp,rdx,rcx,rbx,rax
 *   int_no      <- 由ISR_NOERR/ISR_ERR宏压入
 *   err_code    <- 由ISR_NOERR压入（虚拟0）或CPU压入（ISR_ERR）
 *   rip,cs,rflags,rsp,ss  <- 由CPU压入
 */
struct interrupt_frame {
    uint64_t ds;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

/* 中断处理函数类型 */
typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);

void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t type_attr);
void idt_register_handler(uint8_t num, interrupt_handler_t handler);

/* ISR声明（定义在isr.asm中） */
extern uint64_t isr_table[];

#endif /* IDT_H */
