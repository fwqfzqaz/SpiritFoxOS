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

/* Common interrupt handler called from assembly */
void interrupt_handler(struct interrupt_frame *frame) {
    if (handlers[frame->int_no]) {
        handlers[frame->int_no](frame);
    } else if (frame->int_no >= 32 && frame->int_no < 48) {
        pic_send_eoi(frame->int_no - 32);
    }
}

void idt_init(void) {
    /* Zero out IDT */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt[i].offset_low  = 0;
        idt[i].selector    = 0;
        idt[i].ist         = 0;
        idt[i].type_attr   = 0;
        idt[i].offset_mid  = 0;
        idt[i].offset_high = 0;
        idt[i].reserved    = 0;
    }

    /* Set up ISR gates (0-31: CPU exceptions, 32-255: IRQs/syscalls) */
    for (int i = 0; i < 48; i++) {
        idt_set_gate(i, isr_table[i], 0x08, 0x8E); /* Present, DPL=0, Interrupt Gate */
    }

    /* Load IDTR */
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint64_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));
}
