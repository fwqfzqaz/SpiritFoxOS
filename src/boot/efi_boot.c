/* SpiritFoxOS - UEFI引导程序
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
/* ============================================================
 * SpiritFoxOS EFI Bootloader
 * UEFI entry point - collects system info and jumps to kernel
 *
 * Entry: EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
 * Exit:  Calls kernel_main(bootinfo_t *) and never returns
 *
 * This ENTIRE file is compiled with -mabi=ms (Microsoft x64 calling
 * convention) to match UEFI's ABI on x86_64. All internal function
 * calls use rcx/rdx/r8/r9 parameter passing.
 *
 * EXCEPTION: kernel_main() is declared sysv_abi because it lives in
 * a separate compilation unit (kernel.c) built with default SysV ABI.
 * ============================================================ */

#include <stddef.h>
#include "../include/efi.h"
#include "../include/bootinfo.h"

/* GUID comparisons (simplified) */
static const EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static const EFI_GUID acpi_20_guid = ACPI_20_RSDP_GUID;

/* External kernel entry point (compiled with SysV ABI in kernel.c) */
extern void kernel_main(bootinfo_t *bootinfo) __attribute__((sysv_abi));

/* Kernel start/end from linker */
extern char _start[];
extern char _end[];

/* ============================================================
 * Helper: Compare two GUIDs
 * ============================================================ */
static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    if (a->data1 != b->data1 || a->data2 != b->data2 || a->data3 != b->data3)
        return 0;
    for (int i = 0; i < 8; i++)
        if (a->data4[i] != b->data4[i]) return 0;
    return 1;
}

/* ============================================================
 * BS / Console / GOP wrappers (all EFIAPI for MS ABI indirect calls)
 * ============================================================ */

static EFIAPI EFI_STATUS bs_allocate_pool(EFI_BOOT_SERVICES *bs, uint32_t pool_type,
                                            uint64_t size, void **buffer) {
    typedef EFIAPI EFI_STATUS (*fn_t)(uint32_t, uint64_t, void **);
    fn_t fn = (fn_t)(uintptr_t)bs->allocate_pool;
    return fn(pool_type, size, buffer);
}

static EFIAPI EFI_STATUS bs_free_pool(EFI_BOOT_SERVICES *bs, void *buffer) {
    typedef EFIAPI EFI_STATUS (*fn_t)(void *);
    fn_t fn = (fn_t)(uintptr_t)bs->free_pool;
    return fn(buffer);
}

static EFIAPI EFI_STATUS bs_locate_protocol(EFI_BOOT_SERVICES *bs, const EFI_GUID *guid,
                                              void *registration, void **interface) {
    typedef EFIAPI EFI_STATUS (*fn_t)(const EFI_GUID *, void *, void **);
    fn_t fn = (fn_t)(uintptr_t)bs->locate_protocol;
    return fn(guid, registration, interface);
}

static EFIAPI EFI_STATUS bs_get_memory_map(EFI_BOOT_SERVICES *bs, uint64_t *map_size,
                                             EFI_MEMORY_DESCRIPTOR *map, uint64_t *key,
                                             uint64_t *desc_size, uint32_t *desc_version) {
    typedef EFIAPI EFI_STATUS (*fn_t)(uint64_t *, EFI_MEMORY_DESCRIPTOR *,
                                       uint64_t *, uint64_t *, uint32_t *);
    fn_t fn = (fn_t)(uintptr_t)bs->get_memory_map;
    return fn(map_size, map, key, desc_size, desc_version);
}

static EFIAPI EFI_STATUS bs_exit_boot_services(EFI_BOOT_SERVICES *bs,
                                                 EFI_HANDLE image_handle, uint64_t key) {
    typedef EFIAPI EFI_STATUS (*fn_t)(EFI_HANDLE, uint64_t);
    fn_t fn = (fn_t)(uintptr_t)bs->exit_boot_services;
    return fn(image_handle, key);
}

static EFIAPI EFI_STATUS con_output_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_T *con_out,
                                             uint16_t *string) {
    typedef EFIAPI EFI_STATUS (*fn_t)(void *, uint16_t *);
    fn_t fn = (fn_t)(uintptr_t)con_out->output_string;
    return fn(con_out, string);
}

static EFIAPI EFI_STATUS con_clear_screen(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_T *con_out) {
    typedef EFIAPI EFI_STATUS (*fn_t)(void *);
    fn_t fn = (fn_t)(uintptr_t)con_out->clear_screen;
    return fn(con_out);
}

static EFIAPI EFI_STATUS gop_query_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                          uint32_t mode_num, uint64_t *size_of_info,
                                          EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info) {
    typedef EFIAPI EFI_STATUS (*fn_t)(void *, uint32_t, uint64_t *, void **);
    fn_t fn = (fn_t)(uintptr_t)gop->query_mode;
    return fn(gop, mode_num, size_of_info, (void **)info);
}

static EFIAPI EFI_STATUS gop_set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, uint32_t mode_num) {
    typedef EFIAPI EFI_STATUS (*fn_t)(void *, uint32_t);
    fn_t fn = (fn_t)(uintptr_t)gop->set_mode;
    return fn(gop, mode_num);
}

/* ============================================================
 * Serial output helper (works before/after ExitBootServices)
 * ============================================================ */
static void serial_puts(const char *str) {
    for (const char *p = str; *p; p++) {
        __asm__ volatile("outb %0, %1" : : "a"((unsigned char)*p), "Nd"(0x3F8));
        for (volatile int d = 0; d < 10000; d++);
    }
}

/* ============================================================
 * Print string to UEFI console
 * ============================================================ */
static void efi_print(EFI_SYSTEM_TABLE *st, const char *str) {
    static uint16_t buf[256];
    int i = 0;
    while (str[i] && i < 255) { buf[i] = (uint16_t)str[i]; i++; }
    buf[i] = 0;
    if (st->con_out) con_output_string(st->con_out, buf);
}

/* ============================================================
 * Get memory map from UEFI boot services
 * ============================================================ */
static int get_efi_memory_map(EFI_SYSTEM_TABLE *st, bootinfo_t *bootinfo,
                               uint64_t *mmap_key) {
    EFI_BOOT_SERVICES *bs = st->boot;
    uint64_t mmap_size = 0;
    uint64_t desc_size = 0;
    uint32_t desc_version = 0;
    EFI_STATUS status;

    /* First call: get required buffer size */
    status = bs_get_memory_map(bs, &mmap_size, NULL, mmap_key,
                                &desc_size, &desc_version);

    if (status != EFI_BUFFER_TOO_SMALL && status != EFI_SUCCESS)
        return -1;

    /* Add extra space for new entries between calls */
    mmap_size += desc_size * 4;

    /* Allocate pool for memory map */
    EFI_MEMORY_DESCRIPTOR *mmap = NULL;
    status = bs_allocate_pool(bs, EFI_LOADER_DATA, mmap_size, (void **)&mmap);
    if (status != EFI_SUCCESS) return -1;

    /* Get actual memory map */
    status = bs_get_memory_map(bs, &mmap_size, mmap, mmap_key,
                                &desc_size, &desc_version);
    if (status != EFI_SUCCESS) { bs_free_pool(bs, mmap); return -1; }

    uint64_t entry_count = mmap_size / desc_size;
    bootinfo->mmap_addr = (uint64_t)(uintptr_t)mmap;
    bootinfo->mmap_entry_count = entry_count;
    bootinfo->mmap_entry_size = desc_size;

    return (int)entry_count;
}

/* ============================================================
 * Set up framebuffer via Graphics Output Protocol
 * ============================================================ */
static int setup_framebuffer(EFI_SYSTEM_TABLE *st, bootinfo_t *bootinfo) {
    EFI_BOOT_SERVICES *bs = st->boot;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status;

    status = bs_locate_protocol(bs, &gop_guid, NULL, (void **)&gop);
    if (status != EFI_SUCCESS || !gop) {
        efi_print(st, "[EFI] GOP not available\r\n");
        bootinfo->framebuffer.type = BOOTINFO_FB_TEXT;
        return 0;
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode =
        (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *)gop->mode;

    uint32_t best_mode = mode->mode;
    for (uint32_t i = 0; i < mode->max_mode; i++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        uint64_t info_size = 0;
        status = gop_query_mode(gop, i, &info_size, &info);
        if (status != EFI_SUCCESS || !info) continue;
        if ((info->pixel_format == PIXEL_RED_GREEN_BLUE_RESERVED_8BIT_PER_COLOR ||
             info->pixel_format == PIXEL_BLUE_GREEN_RED_RESERVED_8BIT_PER_COLOR) &&
            info->horizontal_resolution >= 1024 && info->vertical_resolution >= 768) {
            best_mode = i; break;
        }
    }

    status = gop_set_mode(gop, best_mode);
    if (status != EFI_SUCCESS)
        efi_print(st, "[EFI] Using current GOP mode\r\n");

    mode = (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *)gop->mode;
    bootinfo->framebuffer.address = mode->frame_buffer_base;
    bootinfo->framebuffer.pitch = mode->info->horizontal_resolution * 4;
    bootinfo->framebuffer.width = mode->info->horizontal_resolution;
    bootinfo->framebuffer.height = mode->info->vertical_resolution;
    bootinfo->framebuffer.bpp = 32;
    bootinfo->framebuffer.type = BOOTINFO_FB_RGB;

    efi_print(st, "[EFI] Framebuffer initialized\r\n");
    return 0;
}

/* ============================================================
 * Find ACPI RSDP from configuration tables
 * ============================================================ */
static void find_acpi_rsdp(EFI_SYSTEM_TABLE *st, bootinfo_t *bootinfo) {
    uint64_t count = st->number_of_table_entries;
    EFI_CONFIGURATION_TABLE *tables = st->configuration_table;
    for (uint64_t i = 0; i < count; i++) {
        if (guid_eq(&tables[i].vendor_guid, &acpi_20_guid)) {
            bootinfo->acpi.address = (uint64_t)(uintptr_t)tables[i].vendor_table;
            bootinfo->acpi.revision = 2;
            return;
        }
    }
    bootinfo->acpi.address = 0;
    bootinfo->acpi.revision = 0;
}

/* ============================================================
 * Get image base via Loaded Image Protocol
 * ============================================================ */
static uint64_t get_image_base(EFI_HANDLE ImageHandle __attribute__((unused)), EFI_BOOT_SERVICES *bs) {
    static const EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_STATUS status;

    status = bs_locate_protocol(bs, &loaded_image_guid, NULL, (void **)&loaded_image);
    if (status == EFI_SUCCESS && loaded_image && loaded_image->image_base)
        return (uint64_t)(uintptr_t)loaded_image->image_base;

    return 0;
}

/* ============================================================
 * EFI Main Entry Point (EFIAPI = Microsoft x64 calling convention)
 * Called by UEFI firmware when loading this executable.
 *
 * Parameter passing (Microsoft x64 ABI):
 *   rcx = ImageHandle   rdx = SystemTable
 * ============================================================ */
EFIAPI EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {

    EFI_SYSTEM_TABLE *st = SystemTable;
    EFI_BOOT_SERVICES *bs = st->boot;

    /* Validate critical pointers */
    if (!st->con_out) { serial_puts("[ERR] con_out is NULL!\r\n"); return EFI_LOAD_ERROR; }
    if (!bs) { serial_puts("[ERR] bs is NULL!\r\n"); return EFI_LOAD_ERROR; }

    /* Initialize console */
    con_clear_screen(st->con_out);

    efi_print(st, "SpiritFoxOS UEFI Bootloader v1.0\r\n");
    efi_print(st, "================================\r\n\r\n");

    /* Set up bootinfo structure */
    static bootinfo_t bootinfo;
    __builtin_memset(&bootinfo, 0, sizeof(bootinfo));
    bootinfo.magic = BOOTINFO_MAGIC;

    /* Get image base for correct address calculation */
    uint64_t image_base = get_image_base(ImageHandle, bs);
    bootinfo.kernel_start = image_base + 0x1000;
    bootinfo.kernel_end = image_base + 0xB5B18;

    efi_print(st, "[EFI] Kernel image loaded\r\n");

    /* Step 1: Set up framebuffer via GOP */
    efi_print(st, "[EFI] Initializing framebuffer...\r\n");
    setup_framebuffer(st, &bootinfo);

    /* Step 2: Find ACPI RSDP */
    efi_print(st, "[EFI] Locating ACPI tables...\r\n");
    find_acpi_rsdp(st, &bootinfo);

    /* Step 3: Get memory map */
    efi_print(st, "[EFI] Getting memory map...\r\n");
    uint64_t mmap_key = 0;
    int mmap_entries = get_efi_memory_map(st, &bootinfo, &mmap_key);
    if (mmap_entries < 0) {
        efi_print(st, "[EFI] ERROR: Failed to get memory map!\r\n");
        return EFI_LOAD_ERROR;
    }
    efi_print(st, "[EFI] Memory map acquired\r\n");

    /* Step 4: Exit UEFI Boot Services */
    efi_print(st, "[EFI] About to exit boot services...\r\n");
    EFI_STATUS status = bs_exit_boot_services(bs, ImageHandle, mmap_key);
    if (status != EFI_SUCCESS) {
        /* After EBS failure, can't use console anymore */
        serial_puts("[ERR] ExitBootServices failed\r\n");
        return EFI_UNSUPPORTED;
    }

    /* After EBS, must use direct I/O only */
    serial_puts("[EFI] Jumping to kernel\r\n");

    /* Jump to kernel main - never returns */
    kernel_main(&bootinfo);

    return EFI_SUCCESS;
}
