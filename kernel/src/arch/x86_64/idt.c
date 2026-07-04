#include "idt.h"
#include "keyboard.h"
#include "mouse.h"
#include "apic.h"
#include "timer.h"
#include "hal.h"
#include "vga.h"
#include "process.h"
#include "rtl8139.h"
#include "memory.h"
#include "string.h"
#include "serial.h"

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) IDTEntry;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) IDTPtr;

static IDTEntry idt[IDT_ENTRIES];
static IDTPtr   idt_ptr;

/* ISR stub declarations (defined in assembly) */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

/* IRQ stubs (vectors 32-47) */
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

/* Spurious interrupt stub (vector 255) */
extern void irq_spurious(void);

/* Syscall entry point (defined in isr_stub.S) */
extern void syscall_entry(void);

static const char* exception_messages[] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt",
    "Breakpoint", "Into Detected Overflow", "Out of Bounds",
    "Invalid Opcode", "No Coprocessor", "Double Fault",
    "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present",
    "Stack Fault", "General Protection Fault", "Page Fault",
    "Unknown Interrupt", "Coprocessor Fault", "Alignment Check",
    "Machine Check", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved"
};

/* ISR handler - called from assembly for exceptions (INT 0-31) */
void isr_handler(uint64_t int_num, uint64_t error_code)
{
    /* For page faults (vector 14), try COW handling first.
     * Error code bit 1 (W/R) = 1 means write-caused fault.
     * Error code bit 0 (P)   = 1 means protection fault (page present but not writable).
     * This combination is exactly what COW produces. */
    if (int_num == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

        /* Write fault on a present page: (error_code & 0x3) == 0x3 */
        if ((error_code & 0x3) == 0x3) {
            if (process_cow_page_fault(cr2) == 0) {
                /* COW handled successfully - retry the faulting instruction */
                return;
            }
        }
    }

    serial_puts("\n!!! EXCEPTION: ");
    if (int_num < 32) {
        serial_puts(exception_messages[int_num]);
    } else {
        serial_puts("Unknown");
    }
    serial_puts(" [");
    serial_put_hex(int_num);
    serial_puts("] Error code: ");
    serial_put_hex(error_code);

    /* For page faults, print CR2 (faulting address) */
    if (int_num == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        serial_puts(" CR2: ");
        serial_put_hex(cr2);
    }

    serial_puts("\nSystem Halted.\n");

    /* If current process is a user process, kill it instead of halting */
    process_t *cur = process_current();
    if (cur && !(cur->flags & PROC_FLAG_KERNEL) && cur->pid > 0) {
        serial_puts("Killing user process PID=");
        serial_put_dec((uint64_t)cur->pid);
        serial_puts("\n");
        cur->state = PROC_ZOMBIE;
        cur->exit_code = 128 + (int)int_num;
        /* Don't halt - let scheduler continue */
        hal_enable_interrupts();
        for (;;) { __asm__ volatile ("hlt"); }
    }

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* IRQ handler - called from assembly for hardware interrupts */
void irq_handler(uint64_t int_num, uint64_t error_code)
{
    (void)error_code;

    switch (int_num) {
    case 32:  /* Timer (PIT via IOAPIC or LAPIC timer) */
        timer_handler();
        break;
    case 33:  /* Keyboard (IRQ1 via IOAPIC) */
        keyboard_handler();
        break;
    case 44:  /* PS/2 Mouse (IRQ12 via IOAPIC) */
        mouse_handler();
        break;
    case 43:  /* RTL8139 NIC (IRQ11 via IOAPIC) */
        rtl8139_irq_handler();
        break;
    default:
        /* Other IRQs - currently no handler */
        break;
    }

    /* Send EOI: use LAPIC if available, otherwise fall back to PIC */
    apic_eoi();
    if (int_num >= 40) {
        /* Slave PIC EOI (for legacy compatibility) */
        hal_outb(0xA0, 0x20);
    }

    /* Check if rescheduling is needed after timer tick */
    if (int_num == 32 && need_reschedule_check()) {
        scheduler_schedule();
    }
}

/* Spurious interrupt handler */
void spurious_handler(uint64_t int_num, uint64_t error_code)
{
    (void)int_num;
    (void)error_code;
    /* Spurious interrupts must NOT send EOI to IOAPIC,
     * only to LAPIC if it was a real APIC spurious interrupt */
    /* For LAPIC spurious, no EOI is needed */
}

void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t type_attr)
{
    idt[num].offset_low  = handler & 0xFFFF;
    idt[num].selector    = selector;
    idt[num].ist         = 0;
    idt[num].type_attr   = type_attr;
    idt[num].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].reserved    = 0;
}

static void idt_flush(uint64_t idt_ptr_addr)
{
    __asm__ volatile ("lidt (%0)" : : "r"(idt_ptr_addr));
}

void idt_init(void)
{
    idt_ptr.limit = sizeof(IDTEntry) * IDT_ENTRIES - 1;
    idt_ptr.base  = (uint64_t)&idt;

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

    /* Remap PIC: master IRQ 0-7 -> INT 32-39, slave IRQ 8-15 -> INT 40-47
     * This is needed even with APIC to prevent spurious PIC interrupts
     * from triggering CPU exceptions (INT 8-15). PIC will be masked later
     * by apic_init(). */
    hal_outb(0x20, 0x11);  /* ICW1: master, cascade, ICW4 needed */
    hal_outb(0xA0, 0x11);  /* ICW1: slave, cascade, ICW4 needed */
    hal_outb(0x21, 0x20);  /* ICW2: master offset 32 */
    hal_outb(0xA1, 0x28);  /* ICW2: slave offset 40 */
    hal_outb(0x21, 0x04);  /* ICW3: master cascade */
    hal_outb(0xA1, 0x02);  /* ICW3: slave cascade */
    hal_outb(0x21, 0x01);  /* ICW4: 8086 mode */
    hal_outb(0xA1, 0x01);  /* ICW4: 8086 mode */

    /* Mask all PIC IRQs initially - APIC will handle interrupts */
    hal_outb(0x21, 0xFF);
    hal_outb(0xA1, 0xFF);

    /* Set up exception handlers (ISR 0-31) */
    idt_set_gate(0,  (uint64_t)isr0,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(1,  (uint64_t)isr1,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(2,  (uint64_t)isr2,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(3,  (uint64_t)isr3,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(4,  (uint64_t)isr4,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(5,  (uint64_t)isr5,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(6,  (uint64_t)isr6,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(7,  (uint64_t)isr7,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(8,  (uint64_t)isr8,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(9,  (uint64_t)isr9,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(10, (uint64_t)isr10, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(11, (uint64_t)isr11, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(12, (uint64_t)isr12, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(13, (uint64_t)isr13, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(14, (uint64_t)isr14, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(15, (uint64_t)isr15, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(16, (uint64_t)isr16, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(17, (uint64_t)isr17, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(18, (uint64_t)isr18, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(19, (uint64_t)isr19, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(20, (uint64_t)isr20, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(21, (uint64_t)isr21, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(22, (uint64_t)isr22, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(23, (uint64_t)isr23, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(24, (uint64_t)isr24, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(25, (uint64_t)isr25, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(26, (uint64_t)isr26, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(27, (uint64_t)isr27, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(28, (uint64_t)isr28, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(29, (uint64_t)isr29, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(30, (uint64_t)isr30, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(31, (uint64_t)isr31, 0x08, IDT_TYPE_TRAP);

    /* Set up IRQ handlers (vectors 32-47) */
    idt_set_gate(32, (uint64_t)irq0,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(33, (uint64_t)irq1,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(34, (uint64_t)irq2,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(35, (uint64_t)irq3,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(36, (uint64_t)irq4,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(37, (uint64_t)irq5,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(38, (uint64_t)irq6,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(39, (uint64_t)irq7,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(40, (uint64_t)irq8,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(41, (uint64_t)irq9,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(42, (uint64_t)irq10, 0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(43, (uint64_t)irq11, 0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(44, (uint64_t)irq12, 0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(45, (uint64_t)irq13, 0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(46, (uint64_t)irq14, 0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(47, (uint64_t)irq15, 0x08, IDT_TYPE_INTERRUPT);

    /* Spurious interrupt handler (vector 255) */
    idt_set_gate(255, (uint64_t)irq_spurious, 0x08, IDT_TYPE_INTERRUPT);

    /* Load IDT - but do NOT enable interrupts yet (sti).
     * Interrupts will be enabled after APIC is initialized. */
    idt_flush((uint64_t)&idt_ptr);

    /* Set up syscall interface via MSRs */
    /* Enable SYSCALL/SYSRET */
    uint64_t efer = hal_read_msr(MSR_IA32_EFER);
    hal_write_msr(MSR_IA32_EFER, efer | 1);  /* SCE bit */

    /* Set up SYSCALL entry point */
    hal_write_msr(MSR_IA32_STAR, ((uint64_t)0x1B << 48) | ((uint64_t)0x08 << 32));
    hal_write_msr(MSR_IA32_LSTAR, (uint64_t)syscall_entry);
    hal_write_msr(MSR_IA32_FMASK, 0x200);  /* Clear IF on syscall */

    /* Per-CPU scratch space allocation is deferred to syscall_init()
     * because alloc_page() is not available until memory_init() runs.
     * We just set GS_BASE to 0 for now. */
    hal_write_msr(MSR_IA32_GS_BASE, 0);
    hal_write_msr(MSR_IA32_KERNEL_GS_BASE, 0);
}
