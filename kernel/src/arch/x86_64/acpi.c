#include "acpi.h"
#include "hal.h"
#include "string.h"
#include "boot.h"

extern BootInfo *g_boot_info;

static acpi_rsdp_t *rsdp = NULL;
static acpi_sdt_header_t *rsdt = NULL;
static uint32_t rsdt_entry_count = 0;

/* MADT parsed results */
static uintptr_t lapic_addr = 0;
static uintptr_t ioapic_addr = 0;
static uint32_t  ioapic_gsi_base = 0;
static void     *madt_start = NULL;
static uint32_t  madt_length = 0;

static int acpi_checksum(const void *table, size_t length)
{
    const uint8_t *p = (const uint8_t *)table;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++)
        sum += p[i];
    return sum == 0;
}

static acpi_rsdp_t *acpi_scan_for_rsdp(uintptr_t start, uintptr_t end)
{
    for (uintptr_t addr = start; addr < end; addr += 16) {
        hal_ensure_mapped(addr, 16);
        const char *sig = (const char *)addr;
        if (memcmp(sig, "RSD PTR ", 8) != 0)
            continue;
        /* Validate first 20 bytes (ACPI 1.0 checksum) */
        if (!acpi_checksum((const void *)addr, 20))
            continue;
        /* If revision >= 2, also validate extended checksum over full length */
        acpi_rsdp_t *candidate = (acpi_rsdp_t *)addr;
        if (candidate->revision >= 2) {
            hal_ensure_mapped(addr, candidate->length);
            if (!acpi_checksum((const void *)addr, candidate->length))
                continue;
        }
        return candidate;
    }
    return NULL;
}

static acpi_rsdp_t *acpi_find_rsdp(void)
{
    /* Step 1: Search EBDA */
    hal_ensure_mapped(0x40E, 2);
    uint16_t ebda_segment = *(volatile uint16_t *)0x40E;
    uintptr_t ebda_addr = (uintptr_t)ebda_segment << 4;
    if (ebda_addr >= 0x100000)
        ebda_addr = 0; /* sanity check */
    if (ebda_addr != 0) {
        acpi_rsdp_t *found = acpi_scan_for_rsdp(ebda_addr, ebda_addr + 0x400);
        if (found)
            return found;
    }

    /* Step 2: Search 0xE0000 - 0xFFFFF */
    return acpi_scan_for_rsdp(0xE0000, 0x100000);
}

void acpi_init(void)
{
    /* Try RSDP from boot info first (UEFI provides it directly) */
    if (g_boot_info && g_boot_info->acpi_rsdp != 0) {
        uintptr_t rsdp_addr = (uintptr_t)g_boot_info->acpi_rsdp;
        hal_ensure_mapped(rsdp_addr, sizeof(acpi_rsdp_t));
        acpi_rsdp_t *candidate = (acpi_rsdp_t *)rsdp_addr;
        if (memcmp(candidate->signature, "RSD PTR ", 8) == 0) {
            if (candidate->revision >= 2) {
                if (acpi_checksum(candidate, candidate->length)) {
                    rsdp = candidate;
                }
            } else {
                if (acpi_checksum(candidate, 20)) {
                    rsdp = candidate;
                }
            }
        }
    }

    /* Fall back to traditional scanning if RSDP not found via boot info */
    if (!rsdp) {
        rsdp = acpi_find_rsdp();
    }
    
    if (!rsdp)
        return;

    /* Map and validate RSDT */
    hal_ensure_mapped(rsdp->rsdt_address, sizeof(acpi_sdt_header_t));
    rsdt = (acpi_sdt_header_t *)(uintptr_t)rsdp->rsdt_address;

    hal_ensure_mapped(rsdp->rsdt_address, rsdt->length);
    if (!acpi_checksum(rsdt, rsdt->length)) {
        rsdt = NULL;
        return;
    }

    /* Number of 32-bit entries in RSDT (after the header) */
    rsdt_entry_count = (rsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint32_t);

    /* Parse MADT */
    void *madt_ptr = acpi_find_table("APIC");
    if (madt_ptr) {
        acpi_sdt_header_t *madt_hdr = (acpi_sdt_header_t *)madt_ptr;
        madt_start = madt_ptr;
        madt_length = madt_hdr->length;

        /* MADT layout: header (36 bytes) + 4-byte local APIC addr + 4-byte flags + entries */
        if (madt_hdr->length >= sizeof(acpi_sdt_header_t) + 8) {
            uint32_t *madt_fields = (uint32_t *)((uintptr_t)madt_ptr + sizeof(acpi_sdt_header_t));
            lapic_addr = (uintptr_t)madt_fields[0];
        }

        /* Walk MADT entries */
        uintptr_t entry_offset = sizeof(acpi_sdt_header_t) + 8;
        while (entry_offset < madt_hdr->length) {
            madt_entry_t *entry = (madt_entry_t *)((uintptr_t)madt_ptr + entry_offset);
            if (entry->length == 0)
                break; /* prevent infinite loop on malformed data */

            if (entry->type == MADT_TYPE_IOAPIC) {
                madt_ioapic_t *ioapic = (madt_ioapic_t *)entry;
                ioapic_addr = (uintptr_t)ioapic->ioapic_addr;
                ioapic_gsi_base = ioapic->gsi_base;
            }

            entry_offset += entry->length;
        }
    }
}

acpi_rsdp_t *acpi_get_rsdp(void)
{
    return rsdp;
}

void *acpi_find_table(const char *signature)
{
    if (!rsdt || rsdt_entry_count == 0)
        return NULL;

    for (uint32_t i = 0; i < rsdt_entry_count; i++) {
        uint32_t entry_addr = ((uint32_t *)((uintptr_t)rsdt + sizeof(acpi_sdt_header_t)))[i];
        hal_ensure_mapped(entry_addr, sizeof(acpi_sdt_header_t));
        acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)(uintptr_t)entry_addr;
        if (memcmp(hdr->signature, signature, 4) == 0) {
            hal_ensure_mapped(entry_addr, hdr->length);
            if (acpi_checksum(hdr, hdr->length))
                return (void *)hdr;
        }
    }
    return NULL;
}

uintptr_t acpi_get_lapic_addr(void)
{
    return lapic_addr;
}

uintptr_t acpi_get_ioapic_addr(void)
{
    return ioapic_addr;
}

uint32_t acpi_get_ioapic_gsi_base(void)
{
    return ioapic_gsi_base;
}

int acpi_get_irq_override(uint8_t isa_irq, uint32_t *gsi, uint16_t *flags)
{
    if (!madt_start || madt_length == 0)
        return -1;

    uintptr_t entry_offset = sizeof(acpi_sdt_header_t) + 8;
    while (entry_offset < madt_length) {
        madt_entry_t *entry = (madt_entry_t *)((uintptr_t)madt_start + entry_offset);
        if (entry->length == 0)
            break;

        if (entry->type == MADT_TYPE_OVERRIDE) {
            madt_override_t *ovr = (madt_override_t *)entry;
            if (ovr->irq_source == isa_irq) {
                if (gsi)
                    *gsi = ovr->gsi_target;
                if (flags)
                    *flags = ovr->flags;
                return 0;
            }
        }

        entry_offset += entry->length;
    }

    return -1;
}
