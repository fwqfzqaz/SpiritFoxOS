#ifndef IDT_H
#define IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

#define IDT_TYPE_INTERRUPT 0x8E
#define IDT_TYPE_TRAP      0x8F

void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t type_attr);

#endif
