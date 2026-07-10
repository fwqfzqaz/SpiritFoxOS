#ifndef KERNEL_BOOT_H
#define KERNEL_BOOT_H

#include <stdint.h>

#define BOOT_INFO_MAGIC  0x5F1F0F05
#define BOOT_TYPE_LEGACY 0  /* BIOS + GRUB/Multiboot */
#define BOOT_TYPE_UEFI   1  /* UEFI */

typedef struct MemoryMapEntry {
    uint64_t    physical_start;
    uint64_t    virtual_start;
    uint64_t    number_of_pages;
    uint64_t    attribute;
    uint32_t    type;
    uint32_t    reserved;
} MemoryMapEntry;

typedef struct BootInfo {
    uint32_t    magic;
    uint32_t    boot_type;         /* BOOT_TYPE_LEGACY or BOOT_TYPE_UEFI */

    /* Framebuffer info (from VBE or GOP) */
    uint64_t    framebuffer_base;
    uint64_t    framebuffer_size;
    uint32_t    width;
    uint32_t    height;
    uint32_t    pitch;
    uint32_t    bpp;

    /* Memory map info */
    uint64_t    memory_map;
    uint64_t    memory_map_size;
    uint64_t    memory_map_descriptor_size;
    uint64_t    memory_map_entry_count;

    /* ACPI RSDP physical address */
    uint64_t    acpi_rsdp;

    /* UEFI Runtime Services (only valid when boot_type == BOOT_TYPE_UEFI) */
    uint64_t    efi_runtime_services;

    /* Total usable memory in bytes */
    uint64_t    total_memory;
} BootInfo;

#endif /* KERNEL_BOOT_H */
