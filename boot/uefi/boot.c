/*
 * SpiritFoxOS UEFI Bootloader
 * Self-contained - no dependency on gnu-efi
 * All EFI calls go through SystemTable function pointers with ms_abi
 */
#include "boot.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

#define KERNEL_PATH     L"\\EFI\\SpiritFoxOS\\kernel.elf"

typedef void (*kernel_entry_t)(BootInfo *boot_info);

/* ---- Helpers ---- */

static int compare_guid(const EFI_GUID *a, const EFI_GUID *b)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    int i;
    for (i = 0; i < (int)sizeof(EFI_GUID); i++) {
        if (pa[i] != pb[i]) return 1;
    }
    return 0;
}

static void print(EFI_SYSTEM_TABLE *st, const CHAR16 *str)
{
    st->ConOut->OutputString(st->ConOut, (CHAR16 *)str);
}

static void print_hex(EFI_SYSTEM_TABLE *st, uint64_t val)
{
    CHAR16 buf[19]; /* "0x" + 16 hex digits + null */
    static const CHAR16 hex[] = L"0123456789ABCDEF";
    int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        buf[17 - i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[18] = 0;
    print(st, buf);
}

static void print_status(EFI_SYSTEM_TABLE *st, EFI_STATUS s)
{
    print_hex(st, (uint64_t)s);
}

static void print_decimal(EFI_SYSTEM_TABLE *st, uint32_t val)
{
    CHAR16 buf[12];
    int i = 10;
    buf[11] = 0;
    if (val == 0) { print(st, L"0"); return; }
    while (val && i > 0) {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    }
    print(st, &buf[i + 1]);
}

/* ---- Memory Map ---- */

static EFI_STATUS get_memory_map(
    EFI_SYSTEM_TABLE *st,
    EFI_MEMORY_DESCRIPTOR **map, UINTN *map_size,
    UINTN *map_key, UINTN *desc_size, UINT32 *desc_ver)
{
    EFI_STATUS status;

    *map_size = 0;
    *map = NULL;

    status = st->BootServices->GetMemoryMap(
        map_size, *map, map_key, desc_size, desc_ver);
    if (status != EFI_BUFFER_TOO_SMALL)
        return status;

    *map_size += 2 * (*desc_size);

    status = st->BootServices->AllocatePool(
        EfiLoaderData, *map_size, (void **)map);
    if (EFI_ERROR(status))
        return status;

    status = st->BootServices->GetMemoryMap(
        map_size, *map, map_key, desc_size, desc_ver);
    if (EFI_ERROR(status)) {
        st->BootServices->FreePool(*map);
        *map = NULL;
        return status;
    }

    return EFI_SUCCESS;
}

/* ---- GOP ---- */

static EFI_STATUS setup_gop(EFI_SYSTEM_TABLE *st,
                            EFI_GRAPHICS_OUTPUT_PROTOCOL **gop)
{
    EFI_GUID gop_guid = EFI_GOP_GUID;
    return st->BootServices->LocateProtocol(&gop_guid, NULL, (void **)gop);
}

/* ---- Kernel ELF Loader ---- */

static EFI_STATUS load_kernel_elf(
    EFI_SYSTEM_TABLE *st, EFI_HANDLE image_handle,
    void **kernel_entry)
{
    EFI_STATUS status;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_FILE_PROTOCOL *root;
    EFI_FILE_PROTOCOL *kernel_file;
    UINTN file_info_size;
    EFI_FILE_INFO *file_info;
    UINTN kernel_size;
    void *kernel_buffer;
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    UINTN i;

    /* Get loaded image protocol for device handle */
    status = st->BootServices->HandleProtocol(
        image_handle, &li_guid, (void **)&loaded_image);
    if (EFI_ERROR(status)) {
        print(st, L"Failed to get loaded image: ");
        print_status(st, status);
        print(st, L"\r\n");
        return status;
    }

    /* Get file system protocol */
    status = st->BootServices->HandleProtocol(
        loaded_image->DeviceHandle, &fs_guid, (void **)&fs);
    if (EFI_ERROR(status)) {
        print(st, L"Failed to get filesystem: ");
        print_status(st, status);
        print(st, L"\r\n");
        return status;
    }

    /* Open root volume */
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) {
        print(st, L"Failed to open volume: ");
        print_status(st, status);
        print(st, L"\r\n");
        return status;
    }

    /* Open kernel file */
    status = root->FileOpen(root, &kernel_file, KERNEL_PATH,
                        EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        print(st, L"Failed to open kernel: ");
        print_status(st, status);
        print(st, L"\r\n");
        root->FileClose(root);
        return status;
    }

    /* Get file info */
    file_info_size = 0;
    file_info = NULL;
    {
        EFI_GUID fi_guid = EFI_FILE_INFO_GUID;
        status = kernel_file->GetInfo(kernel_file, &fi_guid,
                                      &file_info_size, file_info);
        if (status == EFI_BUFFER_TOO_SMALL) {
            status = st->BootServices->AllocatePool(
                EfiLoaderData, file_info_size, (void **)&file_info);
            if (EFI_ERROR(status)) {
                kernel_file->FileClose(kernel_file);
                root->FileClose(root);
                return status;
            }
            status = kernel_file->GetInfo(kernel_file, &fi_guid,
                                          &file_info_size, file_info);
        }
        if (EFI_ERROR(status)) {
            print(st, L"Failed to get file info: ");
            print_status(st, status);
            print(st, L"\r\n");
            kernel_file->FileClose(kernel_file);
            root->FileClose(root);
            return status;
        }
    }

    kernel_size = file_info->FileSize;
    st->BootServices->FreePool(file_info);

    /* Read kernel into buffer */
    status = st->BootServices->AllocatePool(
        EfiLoaderData, kernel_size, &kernel_buffer);
    if (EFI_ERROR(status)) {
        print(st, L"Failed to allocate buffer: ");
        print_status(st, status);
        print(st, L"\r\n");
        kernel_file->FileClose(kernel_file);
        root->FileClose(root);
        return status;
    }

    status = kernel_file->FileRead(kernel_file, &kernel_size, kernel_buffer);
    kernel_file->FileClose(kernel_file);
    root->FileClose(root);
    if (EFI_ERROR(status)) {
        print(st, L"Failed to read kernel: ");
        print_status(st, status);
        print(st, L"\r\n");
        st->BootServices->FreePool(kernel_buffer);
        return status;
    }

    /* Validate ELF header */
    ehdr = (Elf64_Ehdr *)kernel_buffer;
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        print(st, L"Kernel is not a valid ELF\r\n");
        st->BootServices->FreePool(kernel_buffer);
        return EFI_LOAD_ERROR;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        print(st, L"Kernel is not 64-bit ELF\r\n");
        st->BootServices->FreePool(kernel_buffer);
        return EFI_LOAD_ERROR;
    }

    /* Load program segments */
    phdr = (Elf64_Phdr *)((uint8_t *)kernel_buffer + ehdr->e_phoff);
    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        UINTN pages = (phdr[i].p_memsz + 0xFFF) / 0x1000;
        EFI_PHYSICAL_ADDRESS segment_addr = phdr[i].p_paddr;

        status = st->BootServices->AllocatePages(
            AllocateAddress, EfiLoaderData, pages, &segment_addr);
        if (EFI_ERROR(status)) {
            print(st, L"Failed to alloc segment at 0x");
            print_hex(st, phdr[i].p_paddr);
            print(st, L": ");
            print_status(st, status);
            print(st, L"\r\n");
            st->BootServices->FreePool(kernel_buffer);
            return status;
        }

        /* Copy segment data */
        uint8_t *src = (uint8_t *)kernel_buffer + phdr[i].p_offset;
        uint8_t *dst = (uint8_t *)phdr[i].p_paddr;
        st->BootServices->CopyMem(dst, src, phdr[i].p_filesz);

        /* Zero BSS */
        if (phdr[i].p_memsz > phdr[i].p_filesz) {
            st->BootServices->SetMem(
                dst + phdr[i].p_filesz,
                phdr[i].p_memsz - phdr[i].p_filesz, 0);
        }
    }

    *kernel_entry = (void *)(uintptr_t)ehdr->e_entry;
    st->BootServices->FreePool(kernel_buffer);

    return EFI_SUCCESS;
}

/* ---- ACPI RSDP ---- */

static uint64_t find_acpi_rsdp(EFI_SYSTEM_TABLE *st)
{
    EFI_GUID acpi_20_guid = ACPI_20_TABLE_GUID;
    EFI_GUID acpi_10_guid = ACPI_TABLE_GUID;
    UINTN i;

    /* Prefer ACPI 2.0 */
    for (i = 0; i < st->NumberOfTableEntries; i++) {
        if (compare_guid(&st->ConfigurationTable[i].VendorGuid, &acpi_20_guid))
            return (uint64_t)(uintptr_t)st->ConfigurationTable[i].VendorTable;
    }
    /* Fall back to ACPI 1.0 */
    for (i = 0; i < st->NumberOfTableEntries; i++) {
        if (compare_guid(&st->ConfigurationTable[i].VendorGuid, &acpi_10_guid))
            return (uint64_t)(uintptr_t)st->ConfigurationTable[i].VendorTable;
    }
    return 0;
}

/* ---- Build Boot Info ---- */

static void build_boot_info(
    EFI_SYSTEM_TABLE *st,
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    EFI_MEMORY_DESCRIPTOR *map, UINTN map_size, UINTN desc_size,
    BootInfo *info)
{
    info->magic = BOOT_INFO_MAGIC;
    info->boot_type = BOOT_TYPE_UEFI;

    /* Framebuffer from GOP */
    if (gop != NULL) {
        info->framebuffer_base = gop->Mode->FrameBufferBase;
        info->framebuffer_size = gop->Mode->FrameBufferSize;
        info->width = gop->Mode->Info->HorizontalResolution;
        info->height = gop->Mode->Info->VerticalResolution;
        info->pitch = gop->Mode->Info->PixelsPerScanLine * 4;
        info->bpp = 32;
    } else {
        info->framebuffer_base = 0;
        info->framebuffer_size = 0;
        info->width = 0;
        info->height = 0;
        info->pitch = 0;
        info->bpp = 0;
    }

    /* Memory map */
    info->memory_map = (uint64_t)(uintptr_t)map;
    info->memory_map_size = map_size;
    info->memory_map_descriptor_size = desc_size;
    info->memory_map_entry_count = map_size / desc_size;

    /* ACPI RSDP */
    info->acpi_rsdp = find_acpi_rsdp(st);

    /* UEFI Runtime Services */
    info->efi_runtime_services = (uint64_t)(uintptr_t)st->RuntimeServices;

    /* Total usable memory */
    {
        uint64_t total = 0;
        UINTN n = map_size / desc_size;
        UINTN i;
        for (i = 0; i < n; i++) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(
                (uint8_t *)map + i * desc_size);
            if (d->Type == EfiConventionalMemory)
                total += d->NumberOfPages * 4096;
        }
        info->total_memory = total;
    }
}

/* ---- Main Entry Point ---- */

EFI_STATUS __attribute__((ms_abi)) efi_main(
    EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_MEMORY_DESCRIPTOR *memory_map = NULL;
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    void *kernel_entry = NULL;
    BootInfo boot_info;
    kernel_entry_t kernel;

    print(system_table, L"SpiritFoxOS UEFI Bootloader\r\n");
    print(system_table, L"========================\r\n");

    /* Step 1: GOP */
    print(system_table, L"Setting up GOP...\r\n");
    status = setup_gop(system_table, &gop);
    if (EFI_ERROR(status)) {
        print(system_table, L"GOP not available, no framebuffer\r\n");
        gop = NULL;
    } else {
        print(system_table, L"GOP: ");
        print_decimal(system_table, gop->Mode->Info->HorizontalResolution);
        print(system_table, L"x");
        print_decimal(system_table, gop->Mode->Info->VerticalResolution);
        print(system_table, L" FB=0x");
        print_hex(system_table, gop->Mode->FrameBufferBase);
        print(system_table, L"\r\n");
    }

    /* Step 2: Load kernel */
    print(system_table, L"Loading kernel...\r\n");
    status = load_kernel_elf(system_table, image_handle, &kernel_entry);
    if (EFI_ERROR(status)) {
        print(system_table, L"Kernel load failed!\r\n");
        return status;
    }
    print(system_table, L"Kernel entry: 0x");
    print_hex(system_table, (uint64_t)(uintptr_t)kernel_entry);
    print(system_table, L"\r\n");

    /* Step 3: Memory map */
    print(system_table, L"Getting memory map...\r\n");
    status = get_memory_map(system_table, &memory_map, &map_size,
                            &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(status)) {
        print(system_table, L"Memory map failed!\r\n");
        return status;
    }
    print(system_table, L"Map entries: ");
    print_decimal(system_table, (uint32_t)(map_size / desc_size));
    print(system_table, L"\r\n");

    /* Step 4: Build boot info */
    build_boot_info(system_table, gop, memory_map, map_size,
                    desc_size, &boot_info);

    /* Step 5: Exit boot services (may need retry) */
    print(system_table, L"Exiting boot services...\r\n");
    status = system_table->BootServices->ExitBootServices(
        image_handle, map_key);
    if (EFI_ERROR(status)) {
        /* Stale map key - retry with fresh map */
        system_table->BootServices->FreePool(memory_map);
        status = get_memory_map(system_table, &memory_map, &map_size,
                                &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(status))
            while (1) __asm__ volatile("pause");

        /* Rebuild relevant fields */
        boot_info.memory_map = (uint64_t)(uintptr_t)memory_map;
        boot_info.memory_map_size = map_size;
        boot_info.memory_map_descriptor_size = desc_size;
        boot_info.memory_map_entry_count = map_size / desc_size;
        {
            uint64_t total = 0;
            UINTN n = map_size / desc_size;
            UINTN i;
            for (i = 0; i < n; i++) {
                EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(
                    (uint8_t *)memory_map + i * desc_size);
                if (d->Type == EfiConventionalMemory)
                    total += d->NumberOfPages * 4096;
            }
            boot_info.total_memory = total;
        }

        status = system_table->BootServices->ExitBootServices(
            image_handle, map_key);
        if (EFI_ERROR(status))
            while (1) __asm__ volatile("pause");
    }

    /* Step 6: Jump to kernel */
    kernel = (kernel_entry_t)kernel_entry;
    kernel(&boot_info);

    while (1) __asm__ volatile("pause");
    return EFI_SUCCESS;
}
