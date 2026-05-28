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

/* Interrupt frame (pushed by CPU + our handler) */
struct interrupt_frame {
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
