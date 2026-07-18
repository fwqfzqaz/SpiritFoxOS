/*
 * SpiritFoxOS UEFI 引导加载程序
 * 自包含 - 不依赖 gnu-efi
 * 所有 EFI 调用通过 SystemTable 函数指针进行，使用 ms_abi 调用约定
 *
 * 架构设计原则：
 *   引导层差异化实现，内核层零修改复用
 *   - 所有硬件信息获取、模式切换在引导层完成
 *   - 最终封装为统一的 boot_info 结构体传入内核
 *   - 同一份内核镜像同时支持 BIOS 和 UEFI 两种启动方式
 */
#include "boot.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

#define KERNEL_PATH     L"\\EFI\\SpiritFoxOS\\kernel.elf"

typedef void (*kernel_entry_t)(BootInfo *boot_info);

/* ---- 串口调试输出 (COM1, I/O 端口方式) ---- */
#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init(void)
{
    outb(COM1 + 1, 0x00);    /* 禁用中断 */
    outb(COM1 + 3, 0x80);    /* 启用 DLAB */
    outb(COM1 + 0, 0x03);    /* 波特率除数低字节 = 3 (38400) */
    outb(COM1 + 1, 0x00);    /* 波特率除数高字节 = 0 */
    outb(COM1 + 3, 0x03);    /* 8N1 */
    outb(COM1 + 2, 0xC7);    /* 启用 FIFO, 清空, 14 字节阈值 */
    outb(COM1 + 4, 0x0B);    /* IRQ 启用, RTS/DSR 设置 */
}

static void serial_putc(char c)
{
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, (uint8_t)c);
}

static void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}

static void serial_put_hex(uint64_t val)
{
    const char *hex = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putc(hex[(val >> i) & 0xF]);
}

static void serial_put_decimal(uint32_t val)
{
    char buf[12];
    int i = 10;
    buf[11] = 0;
    if (val == 0) { serial_puts("0"); return; }
    while (val && i > 0) {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    }
    serial_puts(&buf[i + 1]);
}

/* ---- UEFI 控制台输出 ---- */

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

    *map_size += 4 * (*desc_size);  /* 额外空间防止边界变化 */

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
    int kernel_found = 0;

    /* ---- 打开内核文件 ----
     * 策略：遍历所有支持 SimpleFileSystem 协议的句柄，
     * 在每个卷上尝试打开 kernel.elf。
     * 这样无论 EFI 应用是从哪个设备加载的（硬盘、CD-ROM、GRUB chainload），
     * 都能找到内核文件。
     *
     * LocateHandleBuffer 原型:
     *   EFI_STATUS (ms_abi)(int SearchType, EFI_GUID *Protocol,
     *                        void *SearchKey, UINTN *NoHandles, EFI_HANDLE **Buffer)
     * SearchType=ByProtocol (2) 表示查找所有安装了指定协议的句柄。
     */
    {
        /* LocateHandleBuffer 函数指针 - 从 BootServices 表偏移 0x138 读取 */
        typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_LOCATE_HANDLE_BUFFER)(
            int SearchType, EFI_GUID *Protocol, void *SearchKey,
            UINTN *NoHandles, EFI_HANDLE **Buffer);
        EFI_LOCATE_HANDLE_BUFFER LocateHandleBuffer =
            (EFI_LOCATE_HANDLE_BUFFER)st->BootServices->LocateHandleBuffer;

        UINTN num_fs_handles = 0;
        EFI_HANDLE *fs_handles = NULL;
        /* ByProtocol = 2 */
        status = LocateHandleBuffer(2, &fs_guid, NULL,
                                    &num_fs_handles, &fs_handles);
        if (EFI_ERROR(status) || num_fs_handles == 0) {
            print(st, L"No filesystem handles found!\r\n");
            serial_puts("[UEFI] No filesystem handles found!\n");
            return status;
        }

        serial_puts("[UEFI] Found ");
        serial_put_decimal((uint32_t)num_fs_handles);
        serial_puts(" filesystem handle(s)\n");

        for (i = 0; i < num_fs_handles; i++) {
            status = st->BootServices->HandleProtocol(
                fs_handles[i], &fs_guid, (void **)&fs);
            if (EFI_ERROR(status)) continue;

            status = fs->OpenVolume(fs, &root);
            if (EFI_ERROR(status)) continue;

            /* 尝试打开内核文件 */
            status = root->FileOpen(root, &kernel_file, KERNEL_PATH,
                                EFI_FILE_MODE_READ, 0);
            if (EFI_ERROR(status)) {
                serial_puts("[UEFI] kernel not found on FS");
                serial_put_decimal((uint32_t)i);
                serial_puts(" err=0x");
                serial_put_hex((uint64_t)status);
                serial_puts("\n");
                /* 尝试列出根目录内容来调试 */
                {
                    /* 尝试打开 \EFI 目录 */
                    EFI_FILE_PROTOCOL *efi_dir;
                    EFI_STATUS s2 = root->FileOpen(root, &efi_dir,
                        L"\\EFI", EFI_FILE_MODE_READ, 0);
                    if (!EFI_ERROR(s2)) {
                        serial_puts("[UEFI]   \\EFI directory exists\n");
                        /* 尝试多种目录名变体 */
                        const CHAR16 *names[] = {
                            L"SpiritFoxOS", L"spiritfoxos", L"SPIRITFOXOS",
                            L"SPIRIT~1", L"Spirit~1", L"spirit~1", NULL
                        };
                        for (int ni = 0; names[ni]; ni++) {
                            EFI_FILE_PROTOCOL *d;
                            s2 = efi_dir->FileOpen(efi_dir, &d, names[ni],
                                EFI_FILE_MODE_READ, 0);
                            if (!EFI_ERROR(s2)) {
                                serial_puts("[UEFI]   Found dir variant: ");
                                /* 将 CHAR16 转为串口输出 */
                                for (const CHAR16 *p = names[ni]; *p; p++)
                                    serial_putc((char)*p);
                                serial_puts("\n");
                                d->FileClose(d);
                            }
                        }
                        efi_dir->FileClose(efi_dir);
                    } else {
                        serial_puts("[UEFI]   \\EFI not found (0x");
                        serial_put_hex((uint64_t)s2);
                        serial_puts(")\n");
                    }
                }
                root->FileClose(root);
                continue;
            }

            serial_puts("[UEFI] Kernel found on FS");
            serial_put_decimal((uint32_t)i);
            serial_puts("!\n");
            kernel_found = 1;
            break;
        }

        /* 释放句柄缓冲区 */
        if (fs_handles) {
            st->BootServices->FreePool(fs_handles);
        }

        if (!kernel_found) {
            print(st, L"Kernel file not found on any volume!\r\n");
            serial_puts("[UEFI] Kernel file not found on any volume!\n");
            return EFI_NOT_FOUND;
        }
    }

    /* ---- 读取内核文件 ---- */
    serial_puts("[UEFI] Kernel file opened OK\n");

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

    serial_puts("[UEFI] Kernel size: ");
    serial_put_decimal((uint32_t)kernel_size);
    serial_puts(" bytes\n");

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

    serial_puts("[UEFI] ELF header valid, entry=0x");
    serial_put_hex(ehdr->e_entry);
    serial_puts(", segments=");
    serial_put_decimal(ehdr->e_phnum);
    serial_puts("\n");

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
            serial_puts("[UEFI] Failed to alloc segment at 0x");
            serial_put_hex(phdr[i].p_paddr);
            serial_puts(": ");
            serial_put_hex((uint64_t)status);
            serial_puts("\n");
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

    serial_puts("[UEFI] Kernel loaded, entry=0x");
    serial_put_hex((uint64_t)(uintptr_t)*kernel_entry);
    serial_puts("\n");

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
        if (!compare_guid(&st->ConfigurationTable[i].VendorGuid, &acpi_20_guid)) {
            uint64_t rsdp = (uint64_t)(uintptr_t)st->ConfigurationTable[i].VendorTable;
            serial_puts("[UEFI] ACPI 2.0 RSDP at 0x");
            serial_put_hex(rsdp);
            serial_puts("\n");
            return rsdp;
        }
    }
    /* Fall back to ACPI 1.0 */
    for (i = 0; i < st->NumberOfTableEntries; i++) {
        if (!compare_guid(&st->ConfigurationTable[i].VendorGuid, &acpi_10_guid)) {
            uint64_t rsdp = (uint64_t)(uintptr_t)st->ConfigurationTable[i].VendorTable;
            serial_puts("[UEFI] ACPI 1.0 RSDP at 0x");
            serial_put_hex(rsdp);
            serial_puts("\n");
            return rsdp;
        }
    }
    serial_puts("[UEFI] WARNING: No ACPI RSDP found!\n");
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

/*
 * UEFI 内存类型到内核内存类型的转换
 * 关键：退出 Boot Services 后，EfiLoaderCode/Data 和
 * EfiBootServicesCode/Data 都变为可用内存
 */
static uint32_t uefi_mem_type_to_kernel(uint32_t efi_type)
{
    switch (efi_type) {
    case EfiConventionalMemory:
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
        return 7;  /* LOADER_MEM_CONVENTIONAL - 可用内存 */
    case EfiACPIReclaimMemory:
        return 9;  /* ACPI 可回收 */
    case EfiACPIMemoryNVS:
        return 10; /* ACPI NVS */
    case EfiUnusableMemory:
        return 8;  /* 不可用 */
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace:
        return 11; /* MMIO - 保留 */
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:
        return efi_type; /* 保留运行时服务内存 */
    default:
        return 0;  /* 保留 */
    }
}

static void convert_memory_map(
    EFI_MEMORY_DESCRIPTOR *src, UINTN num_entries, UINTN desc_size,
    KernelMemoryMapEntry *dst)
{
    UINTN i;
    for (i = 0; i < num_entries; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR*)(
            (uint8_t *)src + i * desc_size);
        dst[i].physical_start  = d->PhysicalStart;
        dst[i].virtual_start   = d->VirtualStart;
        dst[i].number_of_pages = d->NumberOfPages;
        dst[i].attribute       = d->Attribute;
        dst[i].type            = uefi_mem_type_to_kernel(d->Type);
        dst[i].reserved        = 0;
    }
}

static void build_boot_info(
    EFI_SYSTEM_TABLE *st,
    uint64_t fb_base, uint64_t fb_size,
    uint32_t fb_width, uint32_t fb_height,
    uint32_t fb_pitch, uint32_t fb_bpp,
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
    info->pitch = fb_pitch;
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

    /* Total usable memory - 计算所有 ExitBootServices 后可用的内存 */
    {
        uint64_t total = 0;
        for (i = 0; i < num_entries; i++) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(
                (uint8_t *)raw_map + i * desc_size);
            switch (d->Type) {
            case EfiConventionalMemory:
            case EfiLoaderCode:
            case EfiLoaderData:
            case EfiBootServicesCode:
            case EfiBootServicesData:
                total += d->NumberOfPages * 4096;
                break;
            default:
                break;
            }
        }
        info->total_memory = total;
    }

    serial_puts("[UEFI] BootInfo built: total_memory=");
    serial_put_hex(info->total_memory);
    serial_puts(" fb_base=0x");
    serial_put_hex(fb_base);
    serial_puts(" ");
    serial_put_decimal(fb_width);
    serial_puts("x");
    serial_put_decimal(fb_height);
    serial_puts("x");
    serial_put_decimal(fb_bpp);
    serial_puts("\n");
}

/* ---- 页表构建 ---- */

/*
 * 构建内核需要的恒等映射页表
 * 与 Multiboot 路径的页表布局原则一致，但扩展到 4GB：
 *   P4[0] -> P3[0..3] -> P2[0..3]: 恒等映射 0~4GB (2MB 大页)
 *     P3[0] -> P2[0]: 0~1GB
 *     P3[1] -> P2[1]: 1~2GB
 *     P3[2] -> P2[2]: 2~3GB  (帧缓冲通常在此范围, 如 0x80000000)
 *     P3[3] -> P2[3]: 3~4GB
 *   P4[256] -> P3_hi[0..3] -> P2_hi[0..3]: 高半核映射
 *     物理地址 0~4GB 映射到 0xFFFF800000000000 ~ 0xFFFF803FFFFFFFFF
 *
 * 为什么需要 4GB 映射？
 * UEFI 固件通常将 AllocatePool/AllocatePages 分配的内存放在 2~4GB 范围，
 * 包括 BootInfo 结构体、转换后的内存映射、以及 GOP 帧缓冲 (常见于 0x80000000)。
 * 如果只映射 1GB，切换 CR3 后这些地址不可访问，内核立即 page fault。
 *
 * 注意：UEFI 应用程序自身的代码/数据也在高地址（通常 ~2GB），
 * 切换 CR3 后 CPU 需要从该地址取指，因此必须保证这些地址映射有效。
 * 对于 UEFI 加载地址超过 4GB 的系统，需要改用 trampoline 方案
 * （将切换代码复制到低地址执行）。
 */
static uint64_t p4_table[512] __attribute__((aligned(4096)));
static uint64_t p3_table[512] __attribute__((aligned(4096)));
static uint64_t p2_tables[4][512] __attribute__((aligned(4096)));    /* 恒等映射 0~4GB */
static uint64_t p3_table_hi[512] __attribute__((aligned(4096)));
static uint64_t p2_tables_hi[4][512] __attribute__((aligned(4096))); /* 高半核映射 0~4GB */

static void setup_page_tables(void)
{
    int gb, i;

    /* 清零所有页表 */
    for (i = 0; i < 512; i++) {
        p4_table[i] = 0;
        p3_table[i] = 0;
        p3_table_hi[i] = 0;
    }
    for (gb = 0; gb < 4; gb++) {
        for (i = 0; i < 512; i++) {
            p2_tables[gb][i] = 0;
            p2_tables_hi[gb][i] = 0;
        }
    }

    /* P4[0] -> P3 -> P2[0..3]: 恒等映射 0~4GB (2MB 大页) */
    p4_table[0] = (uint64_t)(uintptr_t)p3_table | 0x03;

    for (gb = 0; gb < 4; gb++) {
        p3_table[gb] = (uint64_t)(uintptr_t)p2_tables[gb] | 0x03;
        for (i = 0; i < 512; i++) {
            uint64_t base = (uint64_t)gb * 0x40000000ULL +
                            (uint64_t)i * 0x200000ULL;
            p2_tables[gb][i] = base | 0x83;  /* PS=1, RW=1, P=1 */
        }
    }

    /* P4[256] -> P3_hi -> P2_hi[0..3]: 高半核映射
     * 物理地址 0~4GB 映射到虚拟地址 0xFFFF800000000000 ~ 0xFFFF803FFFFFFFFF */
    p4_table[256] = (uint64_t)(uintptr_t)p3_table_hi | 0x03;

    for (gb = 0; gb < 4; gb++) {
        p3_table_hi[gb] = (uint64_t)(uintptr_t)p2_tables_hi[gb] | 0x03;
        for (i = 0; i < 512; i++) {
            uint64_t base = (uint64_t)gb * 0x40000000ULL +
                            (uint64_t)i * 0x200000ULL;
            p2_tables_hi[gb][i] = base | 0x83;  /* PS=1, RW=1, P=1 */
        }
    }

    serial_puts("[UEFI] Page tables set up: identity + high-half mapping (0-4GB)\n");
}

/* ---- 跳转到内核 ---- */

/*
 * 跳转到内核入口 _start64
 * 此时已退出 Boot Services，CPU 完全由引导程序控制
 *
 * 关键设计：
 * - GDT 在 inline asm 中直接于栈上构建（低地址 0x800000，恒等映射范围内），
 *   lgdt 和段寄存器加载都访问恒等映射区域，避免 UEFI 高地址映射问题。
 * - CR3 切换后立即切换到内核栈（0x800000），不依赖 UEFI 高地址栈。
 * - 页表映射 0~4GB 恒等映射 + 高半核映射，保证 UEFI 分配的内存仍可访问。
 */
static void __attribute__((noinline)) jump_to_kernel(
    void *kernel_entry, BootInfo *boot_info)
{
    serial_puts("[UEFI] Jumping to kernel at 0x");
    serial_put_hex((uint64_t)(uintptr_t)kernel_entry);
    serial_puts(" with boot_info at 0x");
    serial_put_hex((uint64_t)(uintptr_t)boot_info);
    serial_puts("\n");

    /* 将所有需要的地址存入局部变量，传给 inline asm
     * GDT 在 inline asm 中直接于栈上构建（低地址，恒等映射范围内），
     * 不再使用 UEFI 高地址的 static gdt_entries，避免段寄存器加载时
     * 读取 GDT 描述符的地址映射问题 */
    uint64_t p4_addr = (uint64_t)(uintptr_t)p4_table;

    serial_puts("[UEFI] p4_table at 0x");
    serial_put_hex(p4_addr);
    serial_puts("\n");

    __asm__ volatile (
        /* 关闭中断 */
        "cli\n\t"

        /* 加载内核页表 (CR3) */
        "movq %[p4], %%rax\n\t"
        "movq %%rax, %%cr3\n\t"

        /* 切换到内核栈 (0x800000 在恒等映射范围内) */
        "movq $0x800000, %%rsp\n\t"

        /* ====== 在栈上构建 GDT + 伪描述符 ======
         * 在低地址栈上直接构造 GDT 条目，lgdt 和后续段寄存器
         * 加载都访问恒等映射区域内的地址。
         *
         * 栈布局 (RSP 向低地址增长):
         *   [rsp+0]  : GDT entry 0 (null, 8 bytes)
         *   [rsp+8]  : GDT entry 1 (64-bit code, 8 bytes)
         *   [rsp+16] : GDT entry 2 (data, 8 bytes)
         *   [rsp+24] : 2 字节 limit + 8 字节 base (伪描述符)
         *   总计 34 字节，对齐到 40 字节
         */
        "subq $40, %%rsp\n\t"

        /* GDT entry 0: null descriptor */
        "movq $0, (%%rsp)\n\t"

        /* GDT entry 1: 64-bit code segment (0x00AF9A000000FFFF)
         *  P=1, DPL=0, S=1, Type=exec/read, L=1, D=0, G=1 */
        "movabsq $0x00AF9A000000FFFF, %%rax\n\t"
        "movq %%rax, 8(%%rsp)\n\t"

        /* GDT entry 2: data segment (0x00CF92000000FFFF)
         *  P=1, DPL=0, S=1, Type=r/w, G=1, D/B=1 */
        "movabsq $0x00CF92000000FFFF, %%rax\n\t"
        "movq %%rax, 16(%%rsp)\n\t"

        /* 伪描述符: limit=23 (3 entries * 8 bytes - 1), base=栈上 GDT 地址 */
        "movw $23, 24(%%rsp)\n\t"
        "leaq (%%rsp), %%rax\n\t"
        "movq %%rax, 26(%%rsp)\n\t"

        /* 加载 GDT */
        "lgdt 24(%%rsp)\n\t"

        /* 恢复栈指针 */
        "addq $40, %%rsp\n\t"

        /* 设置数据段选择子 (0x10) */
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"

        /* 将 boot_info 指针放入 %rdi (System V ABI 第一参数) */
        "movq %[bi], %%rdi\n\t"

        /* 远跳转到内核 (通过 lretq 刷新 CS) */
        "pushq $0x08\n\t"                 /* CS = 0x08 (64-bit code segment) */
        "pushq %[entry]\n\t"              /* RIP = kernel_entry */
        "lretq\n\t"

        : /* no output */
        : [p4] "r" (p4_addr),
          [bi] "r" ((uint64_t)(uintptr_t)boot_info),
          [entry] "r" ((uint64_t)(uintptr_t)kernel_entry)
        : "rax", "rdx", "rdi", "memory"
    );

    /* 不应到达此处 */
    while (1) __asm__ volatile("pause");
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

    /* 初始化串口 - 在 UEFI 控制台之前，方便早期调试 */
    serial_init();
    serial_puts("[UEFI] SpiritFoxOS UEFI Bootloader\n");
    serial_puts("[UEFI] ========================\n");

    print(system_table, L"SpiritFoxOS UEFI Bootloader\r\n");
    print(system_table, L"========================\r\n");

    /* 调试: 打印系统表信息 */
    serial_puts("[UEFI] SystemTable at: 0x");
    serial_put_hex((uint64_t)(uintptr_t)system_table);
    serial_puts("\n");
    serial_puts("[UEFI] BootServices at: 0x");
    serial_put_hex((uint64_t)(uintptr_t)system_table->BootServices);
    serial_puts("\n");

    /* 步骤 0: 为转换后的内存映射预分配缓冲区 */
    converted_buf_size = max_entries * sizeof(KernelMemoryMapEntry);
    status = system_table->BootServices->AllocatePool(
        EfiLoaderData, converted_buf_size, (void **)&converted_map);
    if (EFI_ERROR(status)) {
        print(system_table, L"Failed to allocate memory map buffer!\r\n");
        serial_puts("[UEFI] Failed to allocate memory map buffer!\n");
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
    serial_puts("[UEFI] Setting up GOP...\n");
    status = setup_gop(system_table, &gop);
    if (EFI_ERROR(status)) {
        print(system_table, L"GOP not available: ");
        print_status(system_table, status);
        print(system_table, L"\r\n");
        serial_puts("[UEFI] GOP not available: 0x");
        serial_put_hex((uint64_t)status);
        serial_puts("\n");
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

        serial_puts("[UEFI] GOP: ");
        serial_put_decimal(saved_width);
        serial_puts("x");
        serial_put_decimal(saved_height);
        serial_puts(" FB=0x");
        serial_put_hex(saved_fb_base);
        serial_puts(" pitch=");
        serial_put_decimal(saved_pitch);
        serial_puts("\n");
    }

    /* Step 2: Load kernel */
    print(system_table, L"Loading kernel...\r\n");
    serial_puts("[UEFI] Loading kernel...\n");
    status = load_kernel_elf(system_table, image_handle, &kernel_entry);
    if (EFI_ERROR(status)) {
        print(system_table, L"Kernel load failed!\r\n");
        serial_puts("[UEFI] Kernel load failed!\n");
        return status;
    }

    /* 步骤 3: 内存映射 */
    print(system_table, L"Getting memory map...\r\n");
    serial_puts("[UEFI] Getting memory map...\n");
    status = get_memory_map(system_table, &memory_map, &map_size,
                            &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(status)) {
        print(system_table, L"Memory map failed!\r\n");
        serial_puts("[UEFI] Memory map failed!\n");
        return status;
    }
    num_entries = map_size / desc_size;
    print(system_table, L"Map entries: ");
    print_decimal(system_table, (uint32_t)num_entries);
    print(system_table, L"\r\n");
    serial_puts("[UEFI] Map entries: ");
    serial_put_decimal((uint32_t)num_entries);
    serial_puts("\n");

    /* 步骤 3.5: 构建页表（在 ExitBootServices 之前，因为需要分配内存） */
    serial_puts("[UEFI] Setting up page tables...\n");
    setup_page_tables();

    /* 步骤 4: 退出引导服务（可能需要重试） */
    print(system_table, L"Exiting boot services...\r\n");
    serial_puts("[UEFI] Exiting boot services...\n");
    status = system_table->BootServices->ExitBootServices(
        image_handle, map_key);
    if (EFI_ERROR(status)) {
        /* 过期的映射键 - 使用新的映射重试。
         * 在 get_memory_map 和 ExitBootServices 之间不要分配内存！ */
        serial_puts("[UEFI] ExitBootServices key mismatch, retrying...\n");
        system_table->BootServices->FreePool(memory_map);
        status = get_memory_map(system_table, &memory_map, &map_size,
                                &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(status)) {
            serial_puts("[UEFI] FATAL: Cannot get memory map for retry!\n");
            while (1) __asm__ volatile("pause");
        }
        num_entries = map_size / desc_size;

        status = system_table->BootServices->ExitBootServices(
            image_handle, map_key);
        if (EFI_ERROR(status)) {
            serial_puts("[UEFI] FATAL: ExitBootServices failed!\n");
            while (1) __asm__ volatile("pause");
        }
    }

    serial_puts("[UEFI] Boot services exited successfully\n");

    /* 步骤 5: 转换内存映射并构建启动信息（在 ExitBootServices 之后） */
    if (num_entries > max_entries)
        num_entries = max_entries;
    convert_memory_map(memory_map, num_entries, desc_size, converted_map);
    build_boot_info(system_table,
                    saved_fb_base, saved_fb_size,
                    saved_width, saved_height,
                    saved_pitch, saved_bpp,
                    converted_map, num_entries,
                    desc_size, memory_map, boot_info_ptr);

    /* 步骤 6: 跳转到内核 */
    /* 此时已退出 Boot Services，不能使用任何 UEFI 服务（除 Runtime Services）。
     * 切换到内核页表、GDT，然后跳转到 _start64。 */
    serial_puts("[UEFI] Preparing to jump to kernel...\n");
    jump_to_kernel(kernel_entry, boot_info_ptr);

    while (1) __asm__ volatile("pause");
    return EFI_SUCCESS;
}
