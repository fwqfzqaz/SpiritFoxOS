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

/* IDT entry (16 bytes in 64-bit mode) */
struct idt_entry {
    uint16_t offset_low;     /* Offset [15:0] */
    uint16_t selector;       /* Code segment selector */
    uint8_t  ist;            /* IST offset (0 = don't use IST) */
    uint8_t  type_attr;      /* Type and attributes */
    uint16_t offset_mid;     /* Offset [31:16] */
    uint32_t offset_high;    /* Offset [63:32] */
    uint32_t reserved;       /* Must be zero */
} __attribute__((packed));

/* IDTR */
struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Interrupt frame (pushed by CPU + our handler)
 * Stack layout from low to high address (RSP points here):
 *   ds          <- last pushed by isr_common (push rax with ds value)
 *   r15-r8      <- pushed in order: r15 last, r8 first of this group
 *   rdi,rsi,rbp,rdx,rcx,rbx,rax
 *   int_no      <- pushed by ISR_NOERR/ISR_ERR macro
 *   err_code    <- pushed by ISR_NOERR (dummy 0) or CPU (ISR_ERR)
 *   rip,cs,rflags,rsp,ss  <- pushed by CPU
 */
struct interrupt_frame {
    uint64_t ds;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

/* Interrupt handler type */
typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);

void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t type_attr);
void idt_register_handler(uint8_t num, interrupt_handler_t handler);

/* ISR declarations (defined in isr.asm) */
extern uint64_t isr_table[];

#endif /* IDT_H */
