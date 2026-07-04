#ifndef KERNEL_BOOT_H
#define KERNEL_BOOT_H

#include <stdint.h>

#define BOOT_INFO_MAGIC 0x5F1F0F05

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

    /* Framebuffer info */
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
} BootInfo;

#endif /* KERNEL_BOOT_H */
