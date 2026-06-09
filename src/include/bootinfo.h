/* SpiritFoxOS - 启动信息结构
 * Copyright (C) 2025 SpiritFoxOS Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef BOOTINFO_H
#define BOOTINFO_H

#include <stdint.h>

/* ============================================================
 * Unified Boot Information Structure
 * Replaces Multiboot2 info for UEFI boot path.
 * Contains all data the kernel needs from the bootloader/firmware.
 * ============================================================ */

/* Memory map entry - compatible layout with both UEFI and Multiboot2 */
typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} bootinfo_mmap_entry_t;

/* Memory types (matches UEFI and common conventions) */
#define BOOTINFO_MEM_AVAILABLE       1
#define BOOTINFO_MEM_RESERVED        2
#define BOOTINFO_MEM_ACPI_RECLAIMABLE 3
#define BOOTINFO_MEM_NVS             4
#define BOOTINFO_MEM_UNUSABLE        5

/* Framebuffer types */
#define BOOTINFO_FB_RGB     1   /* RGB framebuffer */
#define BOOTINFO_FB_TEXT    2   /* Text mode (no framebuffer) */

/* Framebuffer information */
typedef struct {
    uint64_t address;          /* Physical address of framebuffer */
    uint32_t pitch;            /* Bytes per scanline */
    uint32_t width;            /* Horizontal resolution */
    uint32_t height;           /* Vertical resolution */
    uint8_t  bpp;              /* Bits per pixel (16/24/32) */
    uint8_t  type;             /* FB type: RGB or TEXT */
    uint8_t  reserved[2];
} bootinfo_fb_t;

/* ACPI RSDP pointer */
typedef struct {
    uint64_t address;          /* Physical address of RSDP */
    uint8_t  revision;         /* ACPI revision (1 or 2) */
} bootinfo_acpi_t;

/* Unified boot information - passed to kernel_main */
typedef struct {
    /* Magic number for validation */
    uint32_t magic;

    /* Memory map */
    uint64_t mmap_addr;        /* Physical address of memory map entries */
    uint64_t mmap_entry_count; /* Number of entries in memory map */
    uint64_t mmap_entry_size;  /* Size of each entry (bytes) */

    /* Framebuffer */
    bootinfo_fb_t framebuffer;

    /* ACPI */
    bootinfo_acpi_t acpi;

    /* Kernel image bounds (set by bootloader) */
    uint64_t kernel_start;
    uint64_t kernel_end;

    /* Reserved for future use */
    uint64_t reserved[8];
} bootinfo_t;

/* Bootinfo magic value */
#define BOOTINFO_MAGIC 0x534F584F  /* "SOXO" = SpiritFox OS */

#endif /* BOOTINFO_H */
