#include "apic.h"
#include "acpi.h"
#include "hal.h"
#include "vga.h"

static uintptr_t lapic_base = 0;
static uintptr_t ioapic_base = 0;
static uint32_t  ioapic_gsi_base = 0;
static int       apic_available = 0;

/* Redirect table starts at IOAPIC register 0x10 */
#define IOAPIC_REG_REDIRECTION 0x10

/* ========================================================================
 * Local APIC register access (MMIO)
 * ======================================================================== */

static void lapic_write(uint32_t offset, uint32_t value)
{
    hal_mmio_write32(lapic_base + offset, value);
}

static uint32_t lapic_read(uint32_t offset)
{
    return hal_mmio_read32(lapic_base + offset);
}

/* ========================================================================
 * IOAPIC register access (MMIO, indirect)
 * ======================================================================== */

static void ioapic_write(uint32_t reg, uint32_t value)
{
    hal_mmio_write32(ioapic_base + IOAPIC_IOREGSEL, reg);
    hal_mmio_write32(ioapic_base + IOAPIC_IOWIN, value);
}

static uint32_t ioapic_read(uint32_t reg)
{
    hal_mmio_write32(ioapic_base + IOAPIC_IOREGSEL, reg);
    return hal_mmio_read32(ioapic_base + IOAPIC_IOWIN);
}

/* ========================================================================
 * IOAPIC redirection entry helpers
 * ======================================================================== */

static void ioapic_set_redirection(uint8_t gsi, uint64_t entry)
{
    ioapic_write(IOAPIC_REG_REDIRECTION + gsi * 2, (uint32_t)(entry & 0xFFFFFFFF));
    ioapic_write(IOAPIC_REG_REDIRECTION + gsi * 2 + 1, (uint32_t)(entry >> 32));
}

static uint64_t ioapic_get_redirection(uint8_t gsi)
{
    uint32_t low  = ioapic_read(IOAPIC_REG_REDIRECTION + gsi * 2);
    uint32_t high = ioapic_read(IOAPIC_REG_REDIRECTION + gsi * 2 + 1);
    return ((uint64_t)high << 32) | low;
}

/* ========================================================================
 * Map ISA IRQ to GSI (considering ACPI overrides)
 * ======================================================================== */

static uint32_t irq_to_gsi(uint8_t isa_irq)
{
    uint32_t gsi;
    uint16_t flags;
    if (acpi_get_irq_override(isa_irq, &gsi, &flags) == 0) {
        return gsi;
    }
    /* No override: identity mapping */
    return (uint32_t)isa_irq;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void apic_eoi(void)
{
    if (apic_available) {
        lapic_write(LAPIC_EOI, 0);
    }
}

uint32_t apic_get_lapic_id(void)
{
    if (!apic_available) return 0;
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

uintptr_t apic_get_lapic_base(void)
{
    return lapic_base;
}

void apic_enable_irq(uint8_t isa_irq, uint8_t vector)
{
    if (!apic_available || !ioapic_base) return;

    uint32_t gsi = irq_to_gsi(isa_irq);

    /* Determine polarity and trigger mode from ACPI override */
    uint16_t override_flags = 0;
    uint32_t tmp_gsi;
    acpi_get_irq_override(isa_irq, &tmp_gsi, &override_flags);

    uint64_t entry = (uint64_t)vector;                    /* Vector */
    entry |= IOAPIC_DELMOD_FIXED;                         /* Fixed delivery */
    entry |= IOAPIC_DESTMOD_PHYS;                         /* Physical destination */

    /* Polarity: bit 1 of flags (0 = active high, 1 = active low) */
    if (override_flags & 0x02) {
        entry |= IOAPIC_POLARITY_LOW;
    }

    /* Trigger mode: bit 0 of flags (0 = edge, 1 = level) */
    if (override_flags & 0x01) {
        entry |= IOAPIC_TRIGGER_LEVEL;
    }

    /* Destination: BSP APIC ID */
    uint32_t dest_id = apic_get_lapic_id();
    entry |= ((uint64_t)dest_id << 56);

    ioapic_set_redirection(gsi - ioapic_gsi_base, entry);
}

void apic_disable_irq(uint8_t isa_irq)
{
    if (!apic_available || !ioapic_base) return;

    uint32_t gsi = irq_to_gsi(isa_irq);
    uint64_t entry = ioapic_get_redirection(gsi - ioapic_gsi_base);
    entry |= IOAPIC_MASK;
    ioapic_set_redirection(gsi - ioapic_gsi_base, entry);
}

void apic_init(void)
{
    /* Step 1: Get LAPIC base address from MSR */
    uint64_t msr_val = hal_read_msr(MSR_IA32_APIC_BASE);
    lapic_base = (uintptr_t)(msr_val & APIC_BASE_ADDR_MASK);

    /* Try ACPI first for LAPIC address */
    uintptr_t acpi_lapic = acpi_get_lapic_addr();
    if (acpi_lapic != 0) {
        lapic_base = acpi_lapic;
    }

    if (lapic_base == 0) {
        printf("[APIC] ERROR: No LAPIC address found!\n");
        return;
    }

    printf("[APIC] LAPIC base: %p\n", (void*)lapic_base);

    /* Step 2: Map LAPIC MMIO region */
    hal_ensure_mapped(lapic_base, 0x1000);

    /* Step 3: Enable LAPIC globally via MSR */
    if (!(msr_val & APIC_BASE_GLOBAL_ENABLE)) {
        msr_val |= APIC_BASE_GLOBAL_ENABLE;
        hal_write_msr(MSR_IA32_APIC_BASE, msr_val);
    }

    /* Step 4: Set spurious interrupt vector and enable LAPIC */
    lapic_write(LAPIC_SVR, APIC_VECTOR_SPURIOUS | LAPIC_SVR_ENABLE);

    /* Step 5: Clear error status register */
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    /* Step 6: Mask all LVT entries initially */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);

    /* Step 7: Set Task Priority to 0 (accept all interrupts) */
    lapic_write(LAPIC_TPR, 0);

    apic_available = 1;

    printf("[APIC] LAPIC ID: %u, Version: 0x%x\n",
           apic_get_lapic_id(), lapic_read(LAPIC_VERSION));

    /* Step 8: Initialize IOAPIC */
    ioapic_base = acpi_get_ioapic_addr();
    ioapic_gsi_base = acpi_get_ioapic_gsi_base();

    if (ioapic_base == 0) {
        printf("[APIC] WARNING: No IOAPIC found, using LAPIC only\n");
        return;
    }

    /* Map IOAPIC MMIO region */
    hal_ensure_mapped(ioapic_base, 0x1000);

    uint32_t ioapic_ver = ioapic_read(IOAPIC_REG_VER);
    int max_redir = (ioapic_ver >> 16) & 0xFF;
    printf("[APIC] IOAPIC base: %p, Version: 0x%x, Max redirections: %d\n",
           (void*)ioapic_base, ioapic_ver & 0xFF, max_redir);

    /* Step 9: Mask all IOAPIC redirection entries */
    for (int i = 0; i < max_redir; i++) {
        uint64_t entry = ioapic_get_redirection(i);
        entry |= IOAPIC_MASK;
        ioapic_set_redirection(i, entry);
    }

    /* Step 10: Mask all 8259 PIC interrupts */
    hal_outb(0x21, 0xFF);  /* Mask all master PIC IRQs */
    hal_outb(0xA1, 0xFF);  /* Mask all slave PIC IRQs */

    /* Step 11: Route essential IRQs through IOAPIC.
     * NOTE: We route keyboard IRQ with MASK bit set first,
     * then unmask it after the PS/2 keyboard controller is
     * fully reinitialized. This prevents stale IRQ1 edges
     * from UEFI firmware from being latched by IOAPIC. */
    apic_enable_irq(0, APIC_VECTOR_TIMER);      /* PIT Timer -> GSI 2 -> vector 32 */
    /* Keyboard: set up routing but keep MASKED for now */
    {
        uint32_t gsi = irq_to_gsi(1);
        uint16_t override_flags = 0;
        uint32_t tmp_gsi;
        acpi_get_irq_override(1, &tmp_gsi, &override_flags);
        uint64_t entry = (uint64_t)APIC_VECTOR_KEYBOARD;
        entry |= IOAPIC_DELMOD_FIXED;
        entry |= IOAPIC_DESTMOD_PHYS;
        if (override_flags & 0x02) entry |= IOAPIC_POLARITY_LOW;
        if (override_flags & 0x01) entry |= IOAPIC_TRIGGER_LEVEL;
        entry |= ((uint64_t)apic_get_lapic_id() << 56);
        entry |= IOAPIC_MASK;  /* Keep masked until PS/2 init is done */
        ioapic_set_redirection(gsi - ioapic_gsi_base, entry);
    }
    apic_enable_irq(4, APIC_VECTOR_COM1);       /* COM1 -> vector 36 */
    apic_enable_irq(12, APIC_VECTOR_MOUSE);     /* PS/2 mouse -> vector 44 */
    apic_enable_irq(14, APIC_VECTOR_ATA_PRI);   /* Primary ATA -> vector 46 */
    apic_enable_irq(15, APIC_VECTOR_ATA_SEC);   /* Secondary ATA -> vector 47 */

    /* Reinitialize PS/2 keyboard controller for interrupt mode.
     * UEFI may have left the keyboard in a state where:
     * - The PS/2 controller has pending output data
     * - The keyboard scanning is disabled
     * We must flush all pending data and enable scanning BEFORE
     * unmasking the keyboard IRQ in IOAPIC.
     */
    {
        int timeout;

        /* Step A: Flush ALL pending data from PS/2 controller */
        for (timeout = 0; timeout < 100; timeout++) {
            if (hal_inb(0x64) & 0x01) {
                hal_inb(0x60); /* discard */
                hal_io_wait();
            } else {
                break;
            }
        }

        /* Step B: Ensure keyboard IRQ is enabled in PS/2 CCR */
        timeout = 10000;
        while (timeout-- > 0 && (hal_inb(0x64) & 0x02))
            hal_io_wait();
        hal_outb(0x64, 0x20);
        timeout = 10000;
        while (timeout-- > 0 && (hal_inb(0x64) & 0x01) == 0)
            hal_io_wait();
        uint8_t cmd_byte = hal_inb(0x60);

        cmd_byte |= 0x01;  /* Enable keyboard interrupt (IRQ1) */
        cmd_byte |= 0x40;  /* Translate scan code set 2 to set 1 */
        cmd_byte &= ~0x10; /* Disable keyboard lock */

        timeout = 10000;
        while (timeout-- > 0 && (hal_inb(0x64) & 0x02))
            hal_io_wait();
        hal_outb(0x64, 0x60);
        timeout = 10000;
        while (timeout-- > 0 && (hal_inb(0x64) & 0x02))
            hal_io_wait();
        hal_outb(0x60, cmd_byte);

        /* Step C: Enable keyboard scanning (0xF4).
         * ACK (0xFA) is consumed by polling, not by interrupt,
         * since IOAPIC keyboard entry is still MASKED. */
        timeout = 10000;
        while (timeout-- > 0 && (hal_inb(0x64) & 0x02))
            hal_io_wait();
        hal_outb(0x60, 0xF4);

        timeout = 10000;
        while (timeout-- > 0) {
            if (hal_inb(0x64) & 0x01) {
                hal_inb(0x60); /* consume ACK */
                break;
            }
            hal_io_wait();
        }

        /* Step D: Flush any remaining data (e.g., second ACK byte) */
        hal_io_wait();
        for (timeout = 0; timeout < 100; timeout++) {
            if (hal_inb(0x64) & 0x01) {
                hal_inb(0x60);
                hal_io_wait();
            } else {
                break;
            }
        }

        /* Step E: Now unmask keyboard IRQ in IOAPIC.
         * At this point the PS/2 controller is clean and
         * IRQ1 line is de-asserted, so no stale edges. */
        {
            uint32_t gsi = irq_to_gsi(1);
            uint64_t entry = ioapic_get_redirection(gsi - ioapic_gsi_base);
            entry &= ~IOAPIC_MASK;
            ioapic_set_redirection(gsi - ioapic_gsi_base, entry);
        }
    }

    printf("[APIC] Initialized: PIC disabled, IOAPIC routing active\n");
}
