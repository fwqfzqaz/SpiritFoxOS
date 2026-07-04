#ifndef APIC_H
#define APIC_H

#include <stdint.h>

/* Vector assignments */
#define APIC_VECTOR_TIMER      32
#define APIC_VECTOR_KEYBOARD   33
#define APIC_VECTOR_COM2       35
#define APIC_VECTOR_COM1       36
#define APIC_VECTOR_FLOPPY     38
#define APIC_VECTOR_LPT1       39
#define APIC_VECTOR_RTC        40
#define APIC_VECTOR_MOUSE      44
#define APIC_VECTOR_ATA_PRI    46
#define APIC_VECTOR_ATA_SEC    47
#define APIC_VECTOR_SPURIOUS   255

/* LAPIC register offsets */
#define LAPIC_ID               0x020
#define LAPIC_VERSION          0x030
#define LAPIC_TPR              0x080
#define LAPIC_EOI              0x0B0
#define LAPIC_LDR              0x0D0
#define LAPIC_SVR              0x0F0
#define LAPIC_ESR              0x280
#define LAPIC_LVT_TIMER        0x320
#define LAPIC_LVT_THERMAL      0x330
#define LAPIC_LVT_PERF         0x340
#define LAPIC_LVT_LINT0        0x350
#define LAPIC_LVT_LINT1        0x360
#define LAPIC_LVT_ERROR        0x370
#define LAPIC_TIMER_ICR        0x380
#define LAPIC_TIMER_CCR        0x390
#define LAPIC_TIMER_DCR        0x3E0

/* LAPIC SVR flags */
#define LAPIC_SVR_ENABLE       (1 << 8)

/* LVT timer modes */
#define LAPIC_TIMER_ONE_SHOT   0x00000000
#define LAPIC_TIMER_PERIODIC   0x00020000
#define LAPIC_TIMER_TSC        0x00040000
#define LAPIC_LVT_MASKED       0x00010000

/* IOAPIC register offsets */
#define IOAPIC_IOREGSEL        0x00
#define IOAPIC_IOWIN           0x10

/* IOAPIC registers */
#define IOAPIC_REG_ID          0x00
#define IOAPIC_REG_VER         0x01
#define IOAPIC_REG_ARB         0x02

/* IOAPIC redirection entry flags */
#define IOAPIC_DELMOD_FIXED    (0 << 8)
#define IOAPIC_DELMOD_LOWEST   (1 << 8)
#define IOAPIC_DELMOD_SMI      (2 << 8)
#define IOAPIC_DELMOD_NMI      (4 << 8)
#define IOAPIC_DELMOD_INIT     (5 << 8)
#define IOAPIC_DELMOD_EXTINT   (7 << 8)
#define IOAPIC_DESTMOD_PHYS    (0 << 11)
#define IOAPIC_DESTMOD_LOGIC   (1 << 11)
#define IOAPIC_POLARITY_HIGH   (0 << 13)
#define IOAPIC_POLARITY_LOW    (1 << 13)
#define IOAPIC_TRIGGER_EDGE    (0 << 15)
#define IOAPIC_TRIGGER_LEVEL   (1 << 15)
#define IOAPIC_MASK            (1 << 16)

void apic_init(void);
void apic_eoi(void);
void apic_enable_irq(uint8_t isa_irq, uint8_t vector);
void apic_disable_irq(uint8_t isa_irq);
uint32_t apic_get_lapic_id(void);
uintptr_t apic_get_lapic_base(void);

#endif /* APIC_H */
