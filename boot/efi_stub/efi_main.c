/*
 * SpiritFoxOS EFI Stub - 主引导逻辑
 *
 * Linux bzImage 式 "stub + payload" 模型：本 stub 与内嵌的 kernel.elf 一同
 * 被打包为 kernel.efi，UEFI 固件直接加载它。stub 完成：
 *   1. 从内嵌 payload 解析并加载内核 ELF 段到 0x100000 (含 BSS 清零)
 *   2. 获取 GOP 帧缓冲信息
 *   3. 获取 UEFI 内存映射
 *   4. ExitBootServices
 *   5. 构造统一 BootInfo
 *   6. 建立内核页表 (恒等 + 高半核) 与 GDT
 *   7. 跳转 _start64(entry)，rdi = BootInfo*
 *
 * 所有 EFI 调用通过 SystemTable 函数指针进行 (ms_abi)。
 * 内核层零修改复用 —— 同一份内核镜像仅支持 UEFI 启动。
 */

#include "efi_boot.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* payload.S 提供的内嵌内核 ELF 边界
 *
 * 必须通过 RIP-relative LEA 获取地址。若直接用 extern 引用，编译器在 -fpic
 * 下会经 GOT 访问，而链接脚本丢弃了 .got，导致链接器将符号解析为链接时
 * 绝对地址。UEFI 加载 PE 到不同基址时该绝对地址无效。
 * 内联汇编 lea symbol(%rip) 始终生成位置无关的 PC-relative 代码。
 */
extern const unsigned char __kernel_elf_start[];
extern const unsigned char __kernel_elf_end[];

static inline const uint8_t *get_kernel_elf_start(void)
{
    const uint8_t *p;
    __asm__ volatile ("lea __kernel_elf_start(%%rip), %0" : "=r"(p));
    return p;
}

static inline const uint8_t *get_kernel_elf_end(void)
{
    const uint8_t *p;
    __asm__ volatile ("lea __kernel_elf_end(%%rip), %0" : "=r"(p));
    return p;
}

/* ---- ELF 定义 ---- */
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define ELFMAG0     0x7F
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'
#define EI_CLASS    4
#define ELFCLASS64  2
#define PT_LOAD     1

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

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
    CHAR16 buf[19];
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

/* ---- 内核 ELF 加载 (从内嵌 payload) ---- */

/*
 * 与旧 boot/uefi/boot.c 的 load_kernel_elf 等价，但内核 ELF 已内嵌在本
 * 镜像中 (payload.S)，无需文件系统协议。直接解析 ELF 头，将 PT_LOAD 段
 * 拷贝到其 p_paddr (0x100000)，并清零 BSS。
 */
static EFI_STATUS load_kernel_from_payload(
    EFI_SYSTEM_TABLE *st, void **kernel_entry)
{
    EFI_STATUS status;
    const uint8_t *kernel_buffer = get_kernel_elf_start();
    UINTN kernel_size = (UINTN)(get_kernel_elf_end() - kernel_buffer);
    const Elf64_Ehdr *ehdr;
    const Elf64_Phdr *phdr;
    UINTN i;

    ehdr = (const Elf64_Ehdr *)kernel_buffer;

    /* 校验 ELF 头 */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        print(st, L"Embedded kernel is not a valid ELF\r\n");
        serial_puts("[EFISTUB] Embedded kernel is not a valid ELF\n");
        return EFI_LOAD_ERROR;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        print(st, L"Embedded kernel is not 64-bit ELF\r\n");
        return EFI_LOAD_ERROR;
    }

    serial_puts("[EFISTUB] Embedded kernel ELF: size=");
    serial_put_decimal((uint32_t)kernel_size);
    serial_puts(" entry=0x");
    serial_put_hex(ehdr->e_entry);
    serial_puts(" segments=");
    serial_put_decimal(ehdr->e_phnum);
    serial_puts("\n");

    /* 加载程序段 */
    phdr = (const Elf64_Phdr *)(kernel_buffer + ehdr->e_phoff);
    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        UINTN pages = (phdr[i].p_memsz + 0xFFF) / 0x1000;
        EFI_PHYSICAL_ADDRESS segment_addr = phdr[i].p_paddr;

        /* AllocateAddress: 在指定物理地址分配，确保内核落在链接地址 */
        status = st->BootServices->AllocatePages(
            AllocateAddress, EfiLoaderData, pages, &segment_addr);
        if (EFI_ERROR(status)) {
            serial_puts("[EFISTUB] Failed to alloc segment at 0x");
            serial_put_hex(phdr[i].p_paddr);
            serial_puts(": ");
            serial_put_hex((uint64_t)status);
            serial_puts("\n");
            print(st, L"Failed to alloc segment at 0x");
            print_hex(st, phdr[i].p_paddr);
            print(st, L": ");
            print_status(st, status);
            print(st, L"\r\n");
            return status;
        }

        /* 拷贝段数据 (filesz) */
        const uint8_t *src = kernel_buffer + phdr[i].p_offset;
        uint8_t *dst = (uint8_t *)phdr[i].p_paddr;
        st->BootServices->CopyMem(dst, (void *)src, phdr[i].p_filesz);

        /* 清零 BSS (memsz - filesz) */
        if (phdr[i].p_memsz > phdr[i].p_filesz) {
            st->BootServices->SetMem(
                dst + phdr[i].p_filesz,
                phdr[i].p_memsz - phdr[i].p_filesz, 0);
        }

        serial_puts("[EFISTUB] segment: paddr=0x");
        serial_put_hex(phdr[i].p_paddr);
        serial_puts(" filesz=");
        serial_put_decimal((uint32_t)phdr[i].p_filesz);
        serial_puts(" memsz=");
        serial_put_decimal((uint32_t)phdr[i].p_memsz);
        serial_puts("\n");
    }

    *kernel_entry = (void *)(uintptr_t)ehdr->e_entry;

    serial_puts("[EFISTUB] Kernel loaded, entry=0x");
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
            serial_puts("[EFISTUB] ACPI 2.0 RSDP at 0x");
            serial_put_hex(rsdp);
            serial_puts("\n");
            return rsdp;
        }
    }
    /* 回退到 ACPI 1.0 */
    for (i = 0; i < st->NumberOfTableEntries; i++) {
        if (!compare_guid(&st->ConfigurationTable[i].VendorGuid, &acpi_10_guid)) {
            uint64_t rsdp = (uint64_t)(uintptr_t)st->ConfigurationTable[i].VendorTable;
            serial_puts("[EFISTUB] ACPI 1.0 RSDP at 0x");
            serial_put_hex(rsdp);
            serial_puts("\n");
            return rsdp;
        }
    }
    serial_puts("[EFISTUB] WARNING: No ACPI RSDP found!\n");
    return 0;
}

/* ---- 构建 Boot Info ---- */

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
        return 7;  /* 可用内存 */
    case EfiACPIReclaimMemory:
        return 9;
    case EfiACPIMemoryNVS:
        return 10;
    case EfiUnusableMemory:
        return 8;
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace:
        return 11;
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:
        return efi_type; /* 保留运行时服务内存 */
    default:
        return 0;  /* 保留 */
    }
}

static void convert_memory_map(
    EFI_MEMORY_DESCRIPTOR *src, UINTN num_entries, UINTN desc_size,
    MemoryMapEntry *dst)
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
    MemoryMapEntry *converted_map, UINTN num_entries,
    UINTN desc_size, EFI_MEMORY_DESCRIPTOR *raw_map,
    BootInfo *info)
{
    UINTN i;

    info->magic = BOOT_INFO_MAGIC;
    info->boot_type = BOOT_TYPE_UEFI;

    info->framebuffer_base = fb_base;
    info->framebuffer_size = fb_size;
    info->width = fb_width;
    info->height = fb_height;
    info->pitch = fb_pitch;
    info->bpp = fb_bpp;

    info->memory_map = (uint64_t)(uintptr_t)converted_map;
    info->memory_map_size = num_entries * sizeof(MemoryMapEntry);
    info->memory_map_descriptor_size = sizeof(MemoryMapEntry);
    info->memory_map_entry_count = num_entries;

    info->acpi_rsdp = find_acpi_rsdp(st);
    info->efi_runtime_services = (uint64_t)(uintptr_t)st->RuntimeServices;

    /* EFI stub 不使用 initrd */
    info->initrd_addr = 0;
    info->initrd_size = 0;

    /* 统计 ExitBootServices 后可用内存 */
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

    serial_puts("[EFISTUB] BootInfo: total=");
    serial_put_hex(info->total_memory);
    serial_puts(" fb=0x");
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
 * 构建内核需要的恒等映射页表 (与旧 UEFI bootloader 布局一致, 扩展到 4GB):
 *   P4[0] -> P3[0..3] -> P2[0..3]: 恒等映射 0~4GB (2MB 大页)
 *   P4[256] -> P3_hi[0..3] -> P2_hi[0..3]: 高半核映射 (物理 0~4GB -> 0xFFFF800000000000+)
 *
 * UEFI 通常将 AllocatePool/Pages 分配的内存放在 2~4GB，GOP 帧缓冲常在
 * 0x80000000。切换 CR3 后这些地址必须仍可访问，否则内核立即 page fault。
 */
static uint64_t p4_table[512] __attribute__((aligned(4096)));
static uint64_t p3_table[512] __attribute__((aligned(4096)));
static uint64_t p2_tables[4][512] __attribute__((aligned(4096)));
static uint64_t p3_table_hi[512] __attribute__((aligned(4096)));
static uint64_t p2_tables_hi[4][512] __attribute__((aligned(4096)));

static void setup_page_tables(void)
{
    int gb, i;

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

    p4_table[0] = (uint64_t)(uintptr_t)p3_table | 0x03;
    for (gb = 0; gb < 4; gb++) {
        p3_table[gb] = (uint64_t)(uintptr_t)p2_tables[gb] | 0x03;
        for (i = 0; i < 512; i++) {
            uint64_t base = (uint64_t)gb * 0x40000000ULL +
                            (uint64_t)i * 0x200000ULL;
            p2_tables[gb][i] = base | 0x83;
        }
    }

    p4_table[256] = (uint64_t)(uintptr_t)p3_table_hi | 0x03;
    for (gb = 0; gb < 4; gb++) {
        p3_table_hi[gb] = (uint64_t)(uintptr_t)p2_tables_hi[gb] | 0x03;
        for (i = 0; i < 512; i++) {
            uint64_t base = (uint64_t)gb * 0x40000000ULL +
                            (uint64_t)i * 0x200000ULL;
            p2_tables_hi[gb][i] = base | 0x83;
        }
    }

    serial_puts("[EFISTUB] Page tables set up: identity + high-half (0-4GB)\n");
}

/* ---- 跳转到内核 ---- */

/*
 * 跳转到内核入口 _start64。此时已退出 Boot Services。
 * - GDT 在低地址栈上构建，lgdt 与段寄存器加载都访问恒等映射区域。
 * - CR3 切换后立即切换到内核栈 (0x800000)。
 * - 页表 0~4GB 恒等映射 + 高半核映射，保证 UEFI 分配的内存仍可访问。
 */
static void __attribute__((noinline)) jump_to_kernel(
    void *kernel_entry, BootInfo *boot_info)
{
    serial_puts("[EFISTUB] Jumping to kernel at 0x");
    serial_put_hex((uint64_t)(uintptr_t)kernel_entry);
    serial_puts(" boot_info=0x");
    serial_put_hex((uint64_t)(uintptr_t)boot_info);
    serial_puts("\n");

    uint64_t p4_addr = (uint64_t)(uintptr_t)p4_table;

    __asm__ volatile (
        /* 关中断 */
        "cli\n\t"

        /* 加载内核页表 (CR3) */
        "movq %[p4], %%rax\n\t"
        "movq %%rax, %%cr3\n\t"

        /* 切换到内核栈 (0x800000 在恒等映射范围内) */
        "movq $0x800000, %%rsp\n\t"

        /* ====== 在栈上构建 GDT + 伪描述符 ====== */
        "subq $40, %%rsp\n\t"

        /* GDT entry 0: null */
        "movq $0, (%%rsp)\n\t"

        /* GDT entry 1: 64-bit code (0x00AF9A000000FFFF) */
        "movabsq $0x00AF9A000000FFFF, %%rax\n\t"
        "movq %%rax, 8(%%rsp)\n\t"

        /* GDT entry 2: data (0x00CF92000000FFFF) */
        "movabsq $0x00CF92000000FFFF, %%rax\n\t"
        "movq %%rax, 16(%%rsp)\n\t"

        /* 伪描述符: limit=23, base=栈上 GDT 地址 */
        "movw $23, 24(%%rsp)\n\t"
        "leaq (%%rsp), %%rax\n\t"
        "movq %%rax, 26(%%rsp)\n\t"

        "lgdt 24(%%rsp)\n\t"

        "addq $40, %%rsp\n\t"

        /* 数据段选择子 (0x10) */
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"

        /* boot_info 指针放入 %rdi (System V ABI 第一参数) */
        "movq %[bi], %%rdi\n\t"

        /* 远跳转刷新 CS */
        "pushq $0x08\n\t"
        "pushq %[entry]\n\t"
        "lretq\n\t"

        : /* no output */
        : [p4] "r" (p4_addr),
          [bi] "r" ((uint64_t)(uintptr_t)boot_info),
          [entry] "r" ((uint64_t)(uintptr_t)kernel_entry)
        : "rax", "rdx", "rdi", "memory"
    );

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

    /* BootInfo 分配在专用内存 (非栈上)，避免受栈变化/PE 重定位影响 */
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

    MemoryMapEntry *converted_map = NULL;
    UINTN num_entries;
    UINTN max_entries = 256;
    UINTN converted_buf_size;

    serial_init();
    serial_puts("[EFISTUB] SpiritFoxOS EFI Stub\n");
    serial_puts("[EFISTUB] ========================\n");

    print(system_table, L"SpiritFoxOS EFI Stub\r\n");
    print(system_table, L"========================\r\n");

    serial_puts("[EFISTUB] SystemTable=0x");
    serial_put_hex((uint64_t)(uintptr_t)system_table);
    serial_puts(" BootServices=0x");
    serial_put_hex((uint64_t)(uintptr_t)system_table->BootServices);
    serial_puts("\n");

    /* 步骤 0: 预分配转换后的内存映射缓冲区 */
    converted_buf_size = max_entries * sizeof(MemoryMapEntry);
    status = system_table->BootServices->AllocatePool(
        EfiLoaderData, converted_buf_size, (void **)&converted_map);
    if (EFI_ERROR(status)) {
        print(system_table, L"Failed to allocate memory map buffer!\r\n");
        serial_puts("[EFISTUB] Failed to allocate memory map buffer!\n");
        while (1) __asm__ volatile("pause");
    }

    /* 步骤 1: GOP — 在 ExitBootServices 之前保存帧缓冲信息 */
    uint64_t saved_fb_base = 0, saved_fb_size = 0;
    uint32_t saved_width = 0, saved_height = 0, saved_pitch = 0, saved_bpp = 0;

    print(system_table, L"Setting up GOP...\r\n");
    serial_puts("[EFISTUB] Setting up GOP...\n");
    status = setup_gop(system_table, &gop);
    if (EFI_ERROR(status)) {
        print(system_table, L"GOP not available: ");
        print_status(system_table, status);
        print(system_table, L"\r\n");
        serial_puts("[EFISTUB] GOP not available: 0x");
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

        serial_puts("[EFISTUB] GOP: ");
        serial_put_decimal(saved_width);
        serial_puts("x");
        serial_put_decimal(saved_height);
        serial_puts(" FB=0x");
        serial_put_hex(saved_fb_base);
        serial_puts(" pitch=");
        serial_put_decimal(saved_pitch);
        serial_puts("\n");
    }

    /* 步骤 2: 从内嵌 payload 加载内核 */
    print(system_table, L"Loading kernel...\r\n");
    serial_puts("[EFISTUB] Loading kernel from payload...\n");
    status = load_kernel_from_payload(system_table, &kernel_entry);
    if (EFI_ERROR(status)) {
        print(system_table, L"Kernel load failed!\r\n");
        serial_puts("[EFISTUB] Kernel load failed!\n");
        return status;
    }

    /* 步骤 3: 内存映射 */
    print(system_table, L"Getting memory map...\r\n");
    serial_puts("[EFISTUB] Getting memory map...\n");
    status = get_memory_map(system_table, &memory_map, &map_size,
                            &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(status)) {
        print(system_table, L"Memory map failed!\r\n");
        serial_puts("[EFISTUB] Memory map failed!\n");
        return status;
    }
    num_entries = map_size / desc_size;
    print(system_table, L"Map entries: ");
    print_decimal(system_table, (uint32_t)num_entries);
    print(system_table, L"\r\n");
    serial_puts("[EFISTUB] Map entries: ");
    serial_put_decimal((uint32_t)num_entries);
    serial_puts("\n");

    /* 步骤 3.5: 建立页表 (ExitBootServices 之前) */
    serial_puts("[EFISTUB] Setting up page tables...\n");
    setup_page_tables();

    /* 步骤 4: 退出引导服务 (可能需要重试) */
    print(system_table, L"Exiting boot services...\r\n");
    serial_puts("[EFISTUB] Exiting boot services...\n");
    status = system_table->BootServices->ExitBootServices(
        image_handle, map_key);
    if (EFI_ERROR(status)) {
        serial_puts("[EFISTUB] ExitBootServices key mismatch, retrying...\n");
        system_table->BootServices->FreePool(memory_map);
        status = get_memory_map(system_table, &memory_map, &map_size,
                                &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(status)) {
            serial_puts("[EFISTUB] FATAL: Cannot get memory map for retry!\n");
            while (1) __asm__ volatile("pause");
        }
        num_entries = map_size / desc_size;

        status = system_table->BootServices->ExitBootServices(
            image_handle, map_key);
        if (EFI_ERROR(status)) {
            serial_puts("[EFISTUB] FATAL: ExitBootServices failed!\n");
            while (1) __asm__ volatile("pause");
        }
    }

    serial_puts("[EFISTUB] Boot services exited successfully\n");

    /* 步骤 5: 转换内存映射并构建 BootInfo (ExitBootServices 之后) */
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
    serial_puts("[EFISTUB] Preparing to jump to kernel...\n");
    jump_to_kernel(kernel_entry, boot_info_ptr);

    while (1) __asm__ volatile("pause");
    return EFI_SUCCESS;
}
