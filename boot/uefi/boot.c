/*
 * SpiritFoxOS UEFI 引导加载程序
 * 自包含 - 不依赖 gnu-efi
 * 所有 EFI 调用通过 SystemTable 函数指针进行，使用 ms_abi 调用约定
 */
#include "boot.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

#define KERNEL_PATH     L"\\EFI\\SpiritFoxOS\\kernel.elf"

typedef void (*kernel_entry_t)(BootInfo *boot_info);

/* ---- 辅助函数 ---- */

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

/* ---- 内存映射 ---- */

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

/* ---- 内核 ELF 加载器 ---- */

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

    /* 获取设备句柄的已加载映像协议 */
    print(st, L"  HandleProtocol(loaded_image)...\r\n");
    status = st->BootServices->HandleProtocol(
        image_handle, &li_guid, (void **)&loaded_image);
    if (EFI_ERROR(status)) {
        print(st, L"Failed to get loaded image: ");
        print_status(st, status);
        print(st, L"\r\n");
        return status;
    }
    print(st, L"  loaded_image at: 0x");
    print_hex(st, (uint64_t)(uintptr_t)loaded_image);
    print(st, L"\r\n");
    print(st, L"  DeviceHandle: 0x");
    print_hex(st, (uint64_t)(uintptr_t)loaded_image->DeviceHandle);
    print(st, L"\r\n");

    /* 获取文件系统协议 */
    print(st, L"  HandleProtocol(filesystem)...\r\n");
    status = st->BootServices->HandleProtocol(
        loaded_image->DeviceHandle, &fs_guid, (void **)&fs);
    if (EFI_ERROR(status)) {
        print(st, L"Failed to get filesystem: ");
        print_status(st, status);
        print(st, L"\r\n");
        return status;
    }
    print(st, L"  fs at: 0x");
    print_hex(st, (uint64_t)(uintptr_t)fs);
    print(st, L"\r\n");

    /* Open root volume */
    print(st, L"  OpenVolume...\r\n");
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) {
        print(st, L"Failed to open volume: ");
        print_status(st, status);
        print(st, L"\r\n");
        return status;
    }

    /* 打开内核文件 */
    print(st, L"  Opening kernel file...\r\n");
    status = root->FileOpen(root, &kernel_file, KERNEL_PATH,
                        EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        print(st, L"Failed to open kernel: ");
        print_status(st, status);
        print(st, L" path=");
        print(st, KERNEL_PATH);
        print(st, L"\r\n");
        root->FileClose(root);
        return status;
    }
    print(st, L"  Kernel file opened OK\r\n");

    /* 获取文件信息 */
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

    /* 将内核读入缓冲区 */
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

    /* 加载程序段 */
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

        /* 清零 BSS */
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

    /* 优先使用 ACPI 2.0 */
    for (i = 0; i < st->NumberOfTableEntries; i++) {
        if (!compare_guid(&st->ConfigurationTable[i].VendorGuid, &acpi_20_guid))
            return (uint64_t)(uintptr_t)st->ConfigurationTable[i].VendorTable;
    }
    /* Fall back to ACPI 1.0 */
    for (i = 0; i < st->NumberOfTableEntries; i++) {
        if (!compare_guid(&st->ConfigurationTable[i].VendorGuid, &acpi_10_guid))
            return (uint64_t)(uintptr_t)st->ConfigurationTable[i].VendorTable;
    }
    return 0;
}

/* ---- 构建 Boot Info ---- */

/* 内核的 MemoryMapEntry 布局（与 UEFI EFI_MEMORY_DESCRIPTOR 不同） */
typedef struct {
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
    uint32_t type;
    uint32_t reserved;
} KernelMemoryMapEntry;

static void convert_memory_map(
    EFI_MEMORY_DESCRIPTOR *src, UINTN num_entries, UINTN desc_size,
    KernelMemoryMapEntry *dst)
{
    UINTN i;
    for (i = 0; i < num_entries; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(
            (uint8_t *)src + i * desc_size);
        dst[i].physical_start  = d->PhysicalStart;
        dst[i].virtual_start   = d->VirtualStart;
        dst[i].number_of_pages = d->NumberOfPages;
        dst[i].attribute       = d->Attribute;
        dst[i].type            = d->Type;
        dst[i].reserved        = 0;
    }
}

static void build_boot_info(
    EFI_SYSTEM_TABLE *st,
    uint64_t fb_base, uint64_t fb_size,
    uint32_t fb_width, uint32_t fb_height,
    uint32_t fb_bpp,
    KernelMemoryMapEntry *converted_map, UINTN num_entries,
    UINTN desc_size, EFI_MEMORY_DESCRIPTOR *raw_map,
    BootInfo *info)
{
    UINTN i;

    info->magic = BOOT_INFO_MAGIC;
    info->boot_type = BOOT_TYPE_UEFI;

    /* 帧缓冲信息（在 ExitBootServices 之前保存） */
    info->framebuffer_base = fb_base;
    info->framebuffer_size = fb_size;
    info->width = fb_width;
    info->height = fb_height;
    info->pitch = fb_width * 4;  /* 根据宽度和每像素位数计算 */
    info->bpp = fb_bpp;

    /* 内存映射（已转换） */
    info->memory_map = (uint64_t)(uintptr_t)converted_map;
    info->memory_map_size = num_entries * sizeof(KernelMemoryMapEntry);
    info->memory_map_descriptor_size = sizeof(KernelMemoryMapEntry);
    info->memory_map_entry_count = num_entries;

    /* ACPI RSDP */
    info->acpi_rsdp = find_acpi_rsdp(st);

    /* UEFI 运行时服务 */
    info->efi_runtime_services = (uint64_t)(uintptr_t)st->RuntimeServices;

    /* Total usable memory */
    {
        uint64_t total = 0;
        for (i = 0; i < num_entries; i++) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(
                (uint8_t *)raw_map + i * desc_size);
            if (d->Type == EfiConventionalMemory)
                total += d->NumberOfPages * 4096;
        }
        info->total_memory = total;
    }
}

/* ---- 主入口点 ---- */

EFI_STATUS __attribute__((ms_abi)) efi_main(
    EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_MEMORY_DESCRIPTOR *memory_map = NULL;
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    void *kernel_entry = NULL;
    /* Allocate BootInfo in a safe, dedicated memory location (not on stack).
     * Use AllocatePool to get a dedicated page for BootInfo so it won't
     * be affected by stack changes or PE relocations. */
    BootInfo *boot_info_ptr;
    {
        void *bi_mem;
        status = system_table->BootServices->AllocatePool(
            EfiLoaderData, sizeof(BootInfo), &bi_mem);
        if (EFI_ERROR(status)) {
            print(system_table, L"Failed to allocate BootInfo!\r\n");
            while (1) __asm__ volatile("pause");
        }
        boot_info_ptr = (BootInfo *)bi_mem;
    }
    kernel_entry_t kernel;
    KernelMemoryMapEntry *converted_map = NULL;
    UINTN num_entries;
    UINTN max_entries = 256;  /* 为最坏情况预分配 */
    UINTN converted_buf_size;

    print(system_table, L"SpiritFoxOS UEFI Bootloader\r\n");
    print(system_table, L"========================\r\n");

    /* 调试: 打印系统表信息 */
    print(system_table, L"SystemTable at: 0x");
    print_hex(system_table, (uint64_t)(uintptr_t)system_table);
    print(system_table, L"\r\n");
    print(system_table, L"BootServices at: 0x");
    print_hex(system_table, (uint64_t)(uintptr_t)system_table->BootServices);
    print(system_table, L"\r\n");
    print(system_table, L"ImageHandle: 0x");
    print_hex(system_table, (uint64_t)(uintptr_t)image_handle);
    print(system_table, L"\r\n");

    /* 步骤 0: 为转换后的内存映射预分配缓冲区 */
    converted_buf_size = max_entries * sizeof(KernelMemoryMapEntry);
    status = system_table->BootServices->AllocatePool(
        EfiLoaderData, converted_buf_size, (void **)&converted_map);
    if (EFI_ERROR(status)) {
        print(system_table, L"Failed to allocate memory map buffer!\r\n");
        while (1) __asm__ volatile("pause");
    }

    /* 步骤 1: GOP - 在 ExitBootServices 之前保存帧缓冲信息。
     * ExitBootServices 之后，GOP 协议内存可能被 UEFI 释放，
     * 导致 gop->Mode->Info 指针无效。 */
    uint64_t saved_fb_base = 0;
    uint64_t saved_fb_size = 0;
    uint32_t saved_width = 0;
    uint32_t saved_height = 0;
    uint32_t saved_pitch = 0;
    uint32_t saved_bpp = 0;

    print(system_table, L"Setting up GOP...\r\n");
    status = setup_gop(system_table, &gop);
    if (EFI_ERROR(status)) {
        print(system_table, L"GOP not available: ");
        print_status(system_table, status);
        print(system_table, L"\r\n");
        gop = NULL;
    } else {
        saved_fb_base = gop->Mode->FrameBufferBase;
        saved_fb_size = gop->Mode->FrameBufferSize;
        saved_width  = gop->Mode->Info->HorizontalResolution;
        saved_height = gop->Mode->Info->VerticalResolution;
        saved_pitch  = gop->Mode->Info->PixelsPerScanLine * 4;
        saved_bpp    = 32;

        print(system_table, L"GOP: ");
        print_decimal(system_table, saved_width);
        print(system_table, L"x");
        print_decimal(system_table, saved_height);
        print(system_table, L" FB=0x");
        print_hex(system_table, saved_fb_base);
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

    /* 步骤 3: 内存映射 */
    print(system_table, L"Getting memory map...\r\n");
    status = get_memory_map(system_table, &memory_map, &map_size,
                            &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(status)) {
        print(system_table, L"Memory map failed!\r\n");
        return status;
    }
    num_entries = map_size / desc_size;
    print(system_table, L"Map entries: ");
    print_decimal(system_table, (uint32_t)num_entries);
    print(system_table, L"\r\n");

    /* 步骤 4: 退出引导服务（可能需要重试） */
    print(system_table, L"Exiting boot services...\r\n");
    status = system_table->BootServices->ExitBootServices(
        image_handle, map_key);
    if (EFI_ERROR(status)) {
        /* 过期的映射键 - 使用新的映射重试。
         * 在 get_memory_map 和 ExitBootServices 之间不要分配内存！ */
        system_table->BootServices->FreePool(memory_map);
        status = get_memory_map(system_table, &memory_map, &map_size,
                                &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(status))
            while (1) __asm__ volatile("pause");
        num_entries = map_size / desc_size;

        status = system_table->BootServices->ExitBootServices(
            image_handle, map_key);
        if (EFI_ERROR(status))
            while (1) __asm__ volatile("pause");
    }

    /* 步骤 5: 转换内存映射并构建启动信息（在 ExitBootServices 之后） */
    if (num_entries > max_entries)
        num_entries = max_entries;
    convert_memory_map(memory_map, num_entries, desc_size, converted_map);
    build_boot_info(system_table,
                    saved_fb_base, saved_fb_size,
                    saved_width, saved_height,
                    saved_bpp,
                    converted_map, num_entries,
                    desc_size, memory_map, boot_info_ptr);

    /* 步骤 6: 跳转到内核 */
    kernel = (kernel_entry_t)kernel_entry;
    kernel(boot_info_ptr);

    while (1) __asm__ volatile("pause");
    return EFI_SUCCESS;
}
