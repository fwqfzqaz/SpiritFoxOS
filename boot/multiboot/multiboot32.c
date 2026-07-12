/*
 * multiboot32.c - SpiritFoxOS 的 32 位 multiboot2 加载器
 * GRUB 将此作为 ELF32 multiboot2 内核加载。
 * 它设置长模式并跳转到作为模块加载的 64 位内核。
 *
 * Multiboot2 通过标签提供正确的 VBE 帧缓冲支持。
 */

#include <stdint.h>
#include <stddef.h>

/* ========== Multiboot2 头部 ========== */
#define MB2_MAGIC       0xE85250D6
#define MB2_ARCHITECTURE_I386  0  /* 0 = i386（包括 x86_64） */
#define MB2_HEADER_TAG_FRAMEBUFFER  5
#define MB2_HEADER_TAG_END          0

/* Multiboot2 头部标签 */
struct mb2_header_tag {
    uint16_t type;
    uint16_t flags;
    uint32_t size;
};

struct mb2_header_tag_fb {
    uint16_t type;      /* 5 = 帧缓冲 */
    uint16_t flags;
    uint32_t size;
    uint32_t width;     /* 请求的宽度 */
    uint32_t height;    /* 请求的高度 */
    uint32_t depth;     /* 请求的每像素位数 */
};

/* Multiboot2 头部 - 必须 8 字节对齐。
 * 帧缓冲通过 GRUB gfxpayload 请求，而非头部标签
 * （GRUB 2.06 不支持帧缓冲头部标签）。 */
__attribute__((section(".multiboot")))
static const struct {
    uint32_t magic;
    uint32_t architecture;
    uint32_t header_length;
    uint32_t checksum;
    struct mb2_header_tag end_tag;
} mb2_header = {
    .magic = MB2_MAGIC,
    .architecture = MB2_ARCHITECTURE_I386,
    .header_length = sizeof(mb2_header),
    .checksum = -(MB2_MAGIC + MB2_ARCHITECTURE_I386 + sizeof(mb2_header)),
    .end_tag = {
        .type = MB2_HEADER_TAG_END,
        .flags = 0,
        .size = sizeof(struct mb2_header_tag),
    },
};

/* ========== Multiboot2 信息（基于标签） ========== */
#define MB2_BOOTLOADER_MAGIC 0x36D76289

struct mb2_info {
    uint32_t total_size;
    uint32_t reserved;  /* 始终为 0 */
    /* 后面是标签 */
};

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

#define MB2_TAG_END              0
#define MB2_TAG_CMDLINE          1
#define MB2_TAG_BOOTLOADER_NAME  2
#define MB2_TAG_MODULE           3
#define MB2_TAG_BASIC_MEMINFO    4
#define MB2_TAG_BOOTDEV          5
#define MB2_TAG_MMAP             6
#define MB2_TAG_FRAMEBUFFER_INFO 8
#define MB2_TAG_ACPI_OLD         14
#define MB2_TAG_ACPI_NEW         15

struct mb2_tag_module {
    uint32_t type;    /* 3 */
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char string[1];   /* 以空字符结尾 */
};

struct mb2_tag_framebuffer {
    uint32_t type;            /* 8 */
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    /* 后面是 color_info */
};

/* Multiboot2 内存映射标签 */
struct mb2_tag_mmap {
    uint32_t type;            /* 6 */
    uint32_t size;
    uint32_t entry_size;      /* 每个条目的大小 */
    uint32_t entry_version;   /* 版本，应为 0 */
    /* 后面是条目 */
};

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;            /* 保留 */
};

/* Multiboot2 ACPI 旧/新标签 */
struct mb2_tag_acpi {
    uint32_t type;            /* 14 或 15 */
    uint32_t size;
    /* 后面是 RSDP 数据 */
};

/* ========== 页表 ========== */
static uint64_t p4_table[512] __attribute__((aligned(4096)));
static uint64_t p3_table[512] __attribute__((aligned(4096)));
static uint64_t p2_table[512] __attribute__((aligned(4096)));

/* ========== 64 位内核的 BootInfo ========== */
struct BootInfo64 {
    uint32_t magic;
    uint32_t boot_type;         /* BOOT_TYPE_LEGACY = 0 */
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint64_t memory_map;
    uint64_t memory_map_size;
    uint64_t memory_map_descriptor_size;
    uint64_t memory_map_entry_count;
    uint64_t acpi_rsdp;         /* 暂为 0，由内核扫描 */
    uint64_t efi_runtime_services; /* 传统引导下为 0 */
    uint64_t total_memory;
};

#define BOOTINFO_ADDR 0x9000

/* ========== Bochs VBE LFB 地址 ========== */
/* QEMU 标准 VGA 帧缓冲基地址 - 仅作为回退默认值 */
#define VBE_LFB_PHYS_ADDR  0xE0000000

/* ========== 存放 multiboot2 mmap 转换后数据的区域 ========== */
/* 使用 0x500 - 0x3FFF 区域存放转换后的内存映射（最多约 60 条目）
 * 0x400-0x4FF: BDA
 * 0x500-0x7FFF: 通常空闲（传统低地址内存）
 * 0x8000-0x9FFF: AP trampoline 使用
 * 0x9000-0x9FFF: BootInfo 使用
 * 0xA0000+: VGA 文本缓冲区
 * 每个条目 48 字节，0x500-0x3FFF 约 15KB = ~320 个条目
 */
#define MMAP_CONVERT_ADDR  0x500
#define MMAP_CONVERT_MAX   300

/* 内核使用的内存映射条目格式（与 BootInfo MemoryMapEntry 匹配） */
struct loader_mmap_entry {
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
    uint32_t type;
    uint32_t reserved;
};

/* multiboot2 内存类型到 UEFI 类型映射 */
#define LOADER_MEM_RESERVED     0
#define LOADER_MEM_CONVENTIONAL 7

/* VBE DISPI 接口（用于 QEMU/VirtualBox 帧缓冲设置） */
#define VBE_DISPI_IOPORT_INDEX  0x1CE
#define VBE_DISPI_IOPORT_DATA   0x1CF
#define VBE_DISPI_INDEX_ID      0x0
#define VBE_DISPI_INDEX_XRES    0x1
#define VBE_DISPI_INDEX_YRES    0x2
#define VBE_DISPI_INDEX_BPP     0x3
#define VBE_DISPI_INDEX_ENABLE  0x4
#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_LFB_ENABLED   0x40
#define VBE_DISPI_NOCLEARMEM    0x80

/* ========== 串口调试 ========== */
#define COM1 0x3F8

static inline void serial_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t serial_inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void serial_outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t serial_inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init(void)
{
    serial_outb(COM1 + 1, 0x00);
    serial_outb(COM1 + 3, 0x80);
    serial_outb(COM1 + 0, 0x03);
    serial_outb(COM1 + 1, 0x00);
    serial_outb(COM1 + 3, 0x03);
    serial_outb(COM1 + 2, 0xC7);
    serial_outb(COM1 + 4, 0x0B);
}

static void serial_puts(const char *s)
{
    while (*s) {
        while (!(serial_inb(COM1 + 5) & 0x20))
            ;
        serial_outb(COM1, (uint8_t)*s);
        s++;
    }
}

static void serial_put_hex(uint32_t val)
{
    const char *hex = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        serial_outb(COM1, hex[(val >> i) & 0xF]);
    }
}

static void serial_put_hex64(uint64_t val)
{
    const char *hex = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        serial_outb(COM1, hex[(val >> i) & 0xF]);
    }
}

/* ========== 辅助函数：向上对齐到 8 字节 ========== */
static uint32_t align8(uint32_t v)
{
    return (v + 7) & ~7u;
}

/* ========== 入口点 ========== */
extern void enable_long_mode(uint32_t p4_table_addr) __attribute__((noreturn));

void _start_c(uint32_t magic, struct mb2_info *mbi)
    __attribute__((noreturn));

void _start_c(uint32_t magic, struct mb2_info *mbi)
{
    serial_init();
    serial_puts("[LOADER] _start_c reached (multiboot2)\n");

    if (magic != MB2_BOOTLOADER_MAGIC) {
        serial_puts("[LOADER] ERROR: bad magic!\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    serial_puts("[LOADER] mbi=");
    serial_put_hex((uint32_t)mbi);
    if (mbi) {
        serial_puts(" total_size=");
        serial_put_hex(mbi->total_size);
    }
    serial_puts("\n");

    /* ---- 解析 multiboot2 标签 ---- */
    uint32_t kernel_start = 0, kernel_size = 0;
    uint64_t fb_addr = 0;
    uint32_t fb_width = 0, fb_height = 0, fb_pitch = 0, fb_bpp = 0;
    int fb_found = 0;
    uint64_t acpi_rsdp = 0;
    uint32_t mem_lower = 0, mem_upper = 0;

    /* mmap 转换后的条目计数 */
    uint32_t mmap_converted_count = 0;
    uint64_t total_usable_memory = 0;

    if (mbi) {
        uint32_t offset = 8; /* 跳过 total_size 和 reserved */
        uint32_t end = mbi->total_size;

        while (offset < end) {
            struct mb2_tag *tag = (struct mb2_tag *)((uint8_t *)mbi + offset);

            if (tag->type == MB2_TAG_END)
                break;

            switch (tag->type) {
            case MB2_TAG_MODULE: {
                struct mb2_tag_module *mod = (struct mb2_tag_module *)tag;
                if (kernel_start == 0) {
                    kernel_start = mod->mod_start;
                    kernel_size = mod->mod_end - mod->mod_start;
                    serial_puts("[LOADER] module: start=");
                    serial_put_hex(kernel_start);
                    serial_puts(" size=");
                    serial_put_hex(kernel_size);
                    serial_puts("\n");
                }
                break;
            }
            case MB2_TAG_FRAMEBUFFER_INFO: {
                struct mb2_tag_framebuffer *fb = (struct mb2_tag_framebuffer *)tag;
                fb_addr = fb->framebuffer_addr;
                fb_width = fb->framebuffer_width;
                fb_height = fb->framebuffer_height;
                fb_pitch = fb->framebuffer_pitch;
                fb_bpp = fb->framebuffer_bpp;
                fb_found = 1;

                serial_puts("[LOADER] FB tag: base=");
                serial_put_hex64(fb_addr);
                serial_puts(" w=");
                serial_put_hex(fb_width);
                serial_puts(" h=");
                serial_put_hex(fb_height);
                serial_puts(" bpp=");
                serial_put_hex(fb_bpp);
                serial_puts("\n");
                break;
            }
            case MB2_TAG_BASIC_MEMINFO: {
                /* basic_meminfo 标签：uint32_t mem_lower, mem_upper（KB） */
                mem_lower = *(uint32_t *)((uint8_t *)tag + 8);
                mem_upper = *(uint32_t *)((uint8_t *)tag + 12);
                serial_puts("[LOADER] meminfo: lower=");
                serial_put_hex(mem_lower);
                serial_puts("KB upper=");
                serial_put_hex(mem_upper);
                serial_puts("KB\n");
                break;
            }
            case MB2_TAG_MMAP: {
                struct mb2_tag_mmap *mmap_tag = (struct mb2_tag_mmap *)tag;
                uint32_t entry_size = mmap_tag->entry_size;
                uint32_t mmap_data_size = mmap_tag->size - 16;
                uint32_t entry_count = mmap_data_size / entry_size;
                struct mb2_mmap_entry *entries = (struct mb2_mmap_entry *)((uint8_t *)tag + 16);

                serial_puts("[LOADER] mmap: ");
                serial_put_hex(entry_count);
                serial_puts(" entries, entry_size=");
                serial_put_hex(entry_size);
                serial_puts("\n");

                /* 立即转换 multiboot2 mmap 到内核格式，避免内核复制后数据被覆盖 */
                struct loader_mmap_entry *dst = (struct loader_mmap_entry *)MMAP_CONVERT_ADDR;
                for (uint32_t i = 0; i < entry_count && mmap_converted_count < MMAP_CONVERT_MAX; i++) {
                    struct mb2_mmap_entry *src = (struct mb2_mmap_entry *)(
                        (uint8_t *)entries + i * entry_size);

                    dst[mmap_converted_count].physical_start = src->addr;
                    dst[mmap_converted_count].virtual_start  = 0;
                    dst[mmap_converted_count].number_of_pages = src->len / 4096;
                    dst[mmap_converted_count].attribute = 0;
                    dst[mmap_converted_count].reserved = 0;

                    /* multiboot2 类型：1=可用, 2=保留, 3=ACPI可回收, 4=NVS, 5=缺陷内存 */
                    switch (src->type) {
                    case 1:  /* 可用 RAM */
                        dst[mmap_converted_count].type = LOADER_MEM_CONVENTIONAL;
                        total_usable_memory += src->len;
                        break;
                    case 3:  /* ACPI 可回收 */
                        dst[mmap_converted_count].type = 9;
                        break;
                    case 4:  /* NVS */
                        dst[mmap_converted_count].type = 10;
                        break;
                    case 5:  /* 缺陷内存 */
                        dst[mmap_converted_count].type = 11;
                        break;
                    default:
                        dst[mmap_converted_count].type = LOADER_MEM_RESERVED;
                        break;
                    }

                    serial_puts("[LOADER] mmap[");
                    serial_put_hex(mmap_converted_count);
                    serial_puts("]: base=");
                    serial_put_hex64(src->addr);
                    serial_puts(" len=");
                    serial_put_hex64(src->len);
                    serial_puts(" type=");
                    serial_put_hex(src->type);
                    serial_puts("->");
                    serial_put_hex(dst[mmap_converted_count].type);
                    serial_puts("\n");

                    mmap_converted_count++;
                }
                break;
            }
            case MB2_TAG_ACPI_OLD: {
                /* ACPI 1.0 RSDP（标签数据即为 RSDP 本身） */
                struct mb2_tag_acpi *acpi_tag = (struct mb2_tag_acpi *)tag;
                uint32_t rsdp_size = acpi_tag->size - 8; /* 减去 tag header */
                /* 验证 RSDP 签名 */
                const char *sig = (const char *)((uint8_t *)tag + 8);
                if (rsdp_size >= 20 && sig[0] == 'R' && sig[1] == 'S' &&
                    sig[2] == 'D' && sig[3] == ' ' && sig[4] == 'P' &&
                    sig[5] == 'T' && sig[6] == 'R' && sig[7] == ' ') {
                    acpi_rsdp = (uint64_t)(uint32_t)((uint8_t *)tag + 8);
                    serial_puts("[LOADER] ACPI 1.0 RSDP at ");
                    serial_put_hex((uint32_t)acpi_rsdp);
                    serial_puts("\n");
                }
                break;
            }
            case MB2_TAG_ACPI_NEW: {
                /* ACPI 2.0+ RSDP */
                struct mb2_tag_acpi *acpi_tag = (struct mb2_tag_acpi *)tag;
                uint32_t rsdp_size = acpi_tag->size - 8;
                const char *sig = (const char *)((uint8_t *)tag + 8);
                if (rsdp_size >= 36 && sig[0] == 'R' && sig[1] == 'S' &&
                    sig[2] == 'D' && sig[3] == ' ' && sig[4] == 'P' &&
                    sig[5] == 'T' && sig[6] == 'R' && sig[7] == ' ') {
                    acpi_rsdp = (uint64_t)(uint32_t)((uint8_t *)tag + 8);
                    serial_puts("[LOADER] ACPI 2.0 RSDP at ");
                    serial_put_hex((uint32_t)acpi_rsdp);
                    serial_puts("\n");
                }
                break;
            }
            default:
                break;
            }

            /* 前进到下一个标签（8 字节对齐） */
            offset = align8(offset + tag->size);
        }
    }

    if (!kernel_start) {
        serial_puts("[LOADER] No module found!\n");
    }

    /* ---- 将内核模块复制到 0x100000 ---- */
    if (kernel_start && kernel_size) {
        uint8_t *src = (uint8_t *)kernel_start;
        uint8_t *dst = (uint8_t *)0x100000;
        for (uint32_t i = 0; i < kernel_size; i++) {
            dst[i] = src[i];
        }
    }

    /* ---- 设置恒等映射页表 (PAE) ---- */
    for (int i = 0; i < 512; i++) {
        p4_table[i] = 0;
        p3_table[i] = 0;
        p2_table[i] = 0;
    }

    p4_table[0] = (uint64_t)(uint32_t)p3_table | 0x03;
    p3_table[0] = (uint64_t)(uint32_t)p2_table | 0x03;

    for (int i = 0; i < 512; i++) {
        p2_table[i] = (uint64_t)(i * 0x200000) | 0x83;
    }

    serial_puts("[LOADER] Page tables set up\n");

    /* VBE 模式设置推迟到内核的 fb_init() 中执行。
     * 在此处设置会使 VGA 切换到图形模式，导致
     * 文本模式控制台 (0xB8000) 在启动时不可见。 */

    /* ---- 准备 BootInfo ---- */
    struct BootInfo64 *bi = (struct BootInfo64 *)BOOTINFO_ADDR;
    bi->magic = 0x5F1F0F05;
    bi->boot_type = 0;

    if (fb_found && fb_bpp >= 24) {
        /* 如果可用，使用来自 GRUB multiboot2 标签的帧缓冲信息 */
        bi->framebuffer_base = fb_addr;
        bi->width  = fb_width;
        bi->height = fb_height;
        bi->pitch  = fb_pitch;
        bi->bpp    = fb_bpp;
        bi->framebuffer_size = (uint64_t)fb_pitch * fb_height;

        serial_puts("[LOADER] FB: base=");
        serial_put_hex64(bi->framebuffer_base);
        serial_puts(" ");
        serial_put_hex(bi->width);
        serial_puts("x");
        serial_put_hex(bi->height);
        serial_puts("x");
        serial_put_hex(bi->bpp);
        serial_puts("\n");
    } else {
        /* 无帧缓冲标签或分辨率太低 - 尝试使用 VBE 设置图形模式。
         * Bochs DISPI 仅在 QEMU/VirtualBox 中可用，实体机需要 GRUB gfxpayload。 */
        serial_puts("[LOADER] No suitable FB from GRUB, trying VBE DISPI...\n");

        /* 检测 Bochs VBE DISPI 是否可用（仅 QEMU/VirtualBox） */
        serial_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
        uint16_t vbe_id = serial_inw(VBE_DISPI_IOPORT_DATA);
        if (vbe_id != 0xFFFF && vbe_id >= 0xB0C4) {
            /* VBE DISPI 可用 - 设置 1024x768x32 */
            serial_puts("[LOADER] VBE DISPI detected, setting 1024x768x32\n");
            serial_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
            serial_outw(VBE_DISPI_IOPORT_DATA, 0);
            serial_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_XRES);
            serial_outw(VBE_DISPI_IOPORT_DATA, 1024);
            serial_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_YRES);
            serial_outw(VBE_DISPI_IOPORT_DATA, 768);
            serial_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BPP);
            serial_outw(VBE_DISPI_IOPORT_DATA, 32);
            serial_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
            serial_outw(VBE_DISPI_IOPORT_DATA,
                        VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM);

            bi->framebuffer_base = VBE_LFB_PHYS_ADDR;
            bi->width  = 1024;
            bi->height = 768;
            bi->pitch  = 1024 * 4;
            bi->bpp    = 32;
            bi->framebuffer_size = (uint64_t)bi->pitch * bi->height;
        } else {
            /* 实体机且 GRUB 未设置图形模式 - 回退到文本模式 80x25 */
            serial_puts("[LOADER] No VBE DISPI, falling back to text mode\n");
            bi->framebuffer_base = 0xB8000;  /* VGA 文本缓冲区 */
            bi->width  = 80;
            bi->height = 25;
            bi->pitch  = 80 * 2;  /* 每字符 2 字节 */
            bi->bpp    = 4;       /* 标记为文本模式 */
            bi->framebuffer_size = 80 * 25 * 2;
        }
    }

    /* ---- 转换后的 mmap 数据已存放在 MMAP_CONVERT_ADDR ---- */

    bi->memory_map = MMAP_CONVERT_ADDR;
    bi->memory_map_size = mmap_converted_count * sizeof(struct loader_mmap_entry);
    bi->memory_map_descriptor_size = sizeof(struct loader_mmap_entry);
    bi->memory_map_entry_count = mmap_converted_count;
    bi->total_memory = total_usable_memory;

    if (mmap_converted_count > 0) {
        serial_puts("[LOADER] Total usable memory: ");
        serial_put_hex64(total_usable_memory);
        serial_puts(" bytes\n");
    } else {
        /* 无 mmap 标签 - 从 basic_meminfo 估算 */
        bi->memory_map = 0;
        bi->memory_map_size = 0;
        bi->memory_map_descriptor_size = 0;
        bi->memory_map_entry_count = 0;
        bi->total_memory = (uint64_t)mem_upper * 1024;
        serial_puts("[LOADER] No mmap, estimated total: ");
        serial_put_hex64(bi->total_memory);
        serial_puts(" bytes\n");
    }

    bi->acpi_rsdp = acpi_rsdp;
    bi->efi_runtime_services = 0;     /* 传统引导不可用 */

    /* ---- 启用长模式 ---- */
    serial_puts("[LOADER] Calling enable_long_mode...\n");
    __asm__ volatile (
        "movw $0x3FD, %%dx\n\t"
        "1: inb %%dx, %%al; test $0x20, %%al; jz 1b\n\t"
        "movw $0x3F8, %%dx\n\t"
        "movb $'X', %%al; outb %%al, %%dx\n\t"
        ::: "eax", "edx"
    );
    enable_long_mode((uint32_t)p4_table);

    __builtin_unreachable();
}
