/*
 * multiboot32.c - 32-bit multiboot2 loader for SpiritFoxOS
 * GRUB loads this as ELF32 multiboot2 kernel.
 * It sets up long mode and jumps to the 64-bit kernel loaded as a module.
 *
 * Multiboot2 provides proper VBE framebuffer support via tags.
 */

#include <stdint.h>

/* ========== Multiboot2 header ========== */
#define MB2_MAGIC       0xE85250D6
#define MB2_ARCHITECTURE_I386  0  /* 0 = i386 (includes x86_64) */
#define MB2_HEADER_TAG_FRAMEBUFFER  5
#define MB2_HEADER_TAG_END          0

/* Multiboot2 header tags */
struct mb2_header_tag {
    uint16_t type;
    uint16_t flags;
    uint32_t size;
};

struct mb2_header_tag_fb {
    uint16_t type;      /* 5 = framebuffer */
    uint16_t flags;
    uint32_t size;
    uint32_t width;     /* requested width */
    uint32_t height;    /* requested height */
    uint32_t depth;     /* requested bpp */
};

/* Multiboot2 header - must be 8-byte aligned.
 * Framebuffer is requested via GRUB gfxpayload instead of header tag
 * (GRUB 2.06 doesn't support the framebuffer header tag). */
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

/* ========== Multiboot2 info (tag-based) ========== */
#define MB2_BOOTLOADER_MAGIC 0x36D76289

struct mb2_info {
    uint32_t total_size;
    uint32_t reserved;  /* always 0 */
    /* followed by tags */
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

struct mb2_tag_module {
    uint32_t type;    /* 3 */
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char string[1];   /* null-terminated */
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
    /* color_info follows */
};

/* ========== Page tables ========== */
static uint64_t p4_table[512] __attribute__((aligned(4096)));
static uint64_t p3_table[512] __attribute__((aligned(4096)));
static uint64_t p2_table[512] __attribute__((aligned(4096)));

/* ========== BootInfo for 64-bit kernel ========== */
struct BootInfo64 {
    uint32_t magic;
    uint32_t padding;
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
};

#define BOOTINFO_ADDR 0x9000

/* ========== Bochs VBE LFB address ========== */
/* QEMU std VGA framebuffer base address - used by the kernel's fb.c */
#define VBE_LFB_PHYS_ADDR  0xE0000000

/* ========== Serial debug ========== */
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

/* ========== Helper: align up to 8 bytes ========== */
static uint32_t align8(uint32_t v)
{
    return (v + 7) & ~7u;
}

/* ========== Entry point ========== */
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

    /* ---- Parse multiboot2 tags ---- */
    uint32_t kernel_start = 0, kernel_size = 0;
    uint64_t fb_addr = 0;
    uint32_t fb_width = 0, fb_height = 0, fb_pitch = 0, fb_bpp = 0;
    int fb_found = 0;

    if (mbi) {
        uint32_t offset = 8; /* skip total_size and reserved */
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
            default:
                break;
            }

            /* Advance to next tag (8-byte aligned) */
            offset = align8(offset + tag->size);
        }
    }

    if (!kernel_start) {
        serial_puts("[LOADER] No module found!\n");
    }

    /* ---- Copy kernel module to 0x100000 ---- */
    if (kernel_start && kernel_size) {
        uint8_t *src = (uint8_t *)kernel_start;
        uint8_t *dst = (uint8_t *)0x100000;
        for (uint32_t i = 0; i < kernel_size; i++) {
            dst[i] = src[i];
        }
    }

    /* ---- Set up identity-mapped page tables (PAE) ---- */
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

    /* VBE mode setting is deferred to the kernel's fb_init().
     * Setting it here would switch VGA to graphics mode, making
     * the text-mode console (0xB8000) invisible at boot. */

    /* ---- Prepare BootInfo ---- */
    struct BootInfo64 *bi = (struct BootInfo64 *)BOOTINFO_ADDR;
    bi->magic = 0x5F1F0F05;
    bi->padding = 0;

    if (fb_found && fb_bpp >= 24) {
        /* Use framebuffer info from GRUB multiboot2 tags if available */
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
        /* Use Bochs VBE LFB at 0xE0000000 - set by vbe_set_mode() above */
        serial_puts("[LOADER] Using Bochs VBE LFB at 0xE0000000\n");
        bi->framebuffer_base = VBE_LFB_PHYS_ADDR;
        bi->width  = 1024;
        bi->height = 768;
        bi->pitch  = 1024 * 4;  /* 4096 bytes per line (1024 pixels * 4 bytes) */
        bi->bpp    = 32;
        bi->framebuffer_size = (uint64_t)bi->pitch * bi->height;
    }

    bi->memory_map = 0;
    bi->memory_map_size = 0;
    bi->memory_map_descriptor_size = 0;
    bi->memory_map_entry_count = 0;

    /* ---- Enable long mode ---- */
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
