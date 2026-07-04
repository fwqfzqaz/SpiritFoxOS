#ifndef ACPI_H
#define ACPI_H
#include <stdint.h>

typedef struct {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

/* MADT entries */
#define MADT_TYPE_LAPIC    0
#define MADT_TYPE_IOAPIC   1
#define MADT_TYPE_OVERRIDE 2

typedef struct {
    uint8_t  type;
    uint8_t  length;
} __attribute__((packed)) madt_entry_t;

typedef struct {
    uint8_t  type;       /* 0 */
    uint8_t  length;
    uint8_t  processor_id;
    uint8_t  apic_id;
    uint32_t flags;      /* bit 0 = enabled */
} __attribute__((packed)) madt_lapic_t;

typedef struct {
    uint8_t  type;       /* 1 */
    uint8_t  length;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;   /* Global System Interrupt base */
} __attribute__((packed)) madt_ioapic_t;

typedef struct {
    uint8_t  type;       /* 2 */
    uint8_t  length;
    uint8_t  bus_source; /* 0 = ISA */
    uint8_t  irq_source; /* ISA IRQ */
    uint32_t gsi_target; /* Global System Interrupt */
    uint16_t flags;      /* polarity + trigger mode */
} __attribute__((packed)) madt_override_t;

void acpi_init(void);
acpi_rsdp_t* acpi_get_rsdp(void);
void* acpi_find_table(const char *signature);
uintptr_t acpi_get_lapic_addr(void);
uintptr_t acpi_get_ioapic_addr(void);
uint32_t acpi_get_ioapic_gsi_base(void);
int acpi_get_irq_override(uint8_t isa_irq, uint32_t *gsi, uint16_t *flags);

#endif
