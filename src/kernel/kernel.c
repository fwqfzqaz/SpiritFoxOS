#include "../include/bootinfo.h"
#include "../include/efi.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/stddef.h"
#include "../include/stdarg.h"
#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "mouse.h"
#include "pmm.h"
#include "vmm.h"
#include "scheduler.h"
#include "serial.h"
#include "shell.h"
#include "gui.h"
#include "perm.h"
#include "pci.h"
#include "xhci.h"
#include "usb.h"
#include "log.h"
#include "ata.h"
#include "sfs.h"
#include "syscall.h"
#include "devtree.h"
#include "init.h"

/* Kernel start/end symbols from linker script */
extern char _start[];
extern char _end[];

/* Timer tick counter */
volatile uint64_t timer_ticks = 0;

/* Flag: are we in graphical mode? */
static int graphical_mode = 0;

/* Bootinfo framebuffer (saved for GUI init later) */
static bootinfo_fb_t saved_fb_info;

/* Timer interrupt handler */
static void timer_handler(struct interrupt_frame *frame) {
    timer_ticks++;
    scheduler_tick(frame);
    pic_send_eoi(0);
}

/* ============================================================
 * Phase I: 系统内核启动 (System Kernel Boot)
 * I1: 初始化内核栈      - done in boot.asm
 * I2: 初始化MMU          - PMM + VMM
 * I3: 启用分页机制       - done in boot.asm, VMM refines
 * I4: 初始化中断描述符表  - IDT + PIC + PIT
 * I5: 初始化全局描述符表  - GDT + TSS
 * I6: 初始化系统调用接口  - syscall
 * ============================================================ */

static void phase_kernel_boot(bootinfo_t *bootinfo) {
    /* Debug: direct COM1 output ASAP */
    {
        const char *msg = "[PKB] phase_kernel_boot entered\r\n";
        for (const char *p = msg; *p; p++) {
            __asm__ volatile("outb %0, %1" : : "a"((unsigned char)*p), "Nd"(0x3F8));
            for (volatile int d = 0; d < 10000; d++);
        }
    }
    /* Validate bootinfo magic */
    if (bootinfo->magic != BOOTINFO_MAGIC) {
        /* Halt if no valid boot info */
        hlt();
    }

    /* Early debug output */
    serial_init(COM1);
    log_init();
    LOG_I("kernel", "SpiritFoxOS starting (UEFI mode)...");

    /*
     * Parse bootinfo for framebuffer and memory map.
     * The memory map from UEFI uses EFI_MEMORY_DESCRIPTOR entries.
     * We convert them to a format compatible with our PMM.
     */

    /* Extract framebuffer info */
    if (bootinfo->framebuffer.type == BOOTINFO_FB_RGB) {
        graphical_mode = 1;
        __builtin_memcpy(&saved_fb_info, &bootinfo->framebuffer, sizeof(bootinfo_fb_t));
    }

    /*
     * Memory map: bootinfo contains raw EFI memory descriptors.
     * Convert EFI memory types to PMM-compatible types:
     *   EFI_CONVENTIONAL_MEMORY (7) -> AVAILABLE
     *   EFI_UNUSABLE_MEMORY (8)  -> UNUSABLE
     *   EFI_ACPI_RECLAIM_MEMORY (9) -> ACPI_RECLAIMABLE
     *   EFI_ACPI_MEMORY_NVS (10)   -> NVS
     *   Everything else             -> RESERVED
     *
     * We pass the EFI memory map directly; pmm_init_efi() handles conversion.
     */

    /* I2: Initialize Memory Management Unit (PMM + VMM) */
    if (!bootinfo->mmap_addr || !bootinfo->mmap_entry_count) {
        LOG_F("kernel", "No memory map from bootloader!");
        hlt();
    }

    uint64_t kernel_start = bootinfo->kernel_start;
    uint64_t kernel_end = bootinfo->kernel_end;
    pmm_init(bootinfo, kernel_start, kernel_end);
    LOG_I("kernel", "[I2] PMM initialized (kernel %p-%p)", kernel_start, kernel_end);
    /* Debug checkpoint after PMM */
    { const char *m = "[PKB] PMM done\r\n"; for (const char *p = m; *p; p++) { __asm__ volatile("outb %0, %1" : : "a"((unsigned char)*p), "Nd"(0x3F8)); for (volatile int d = 0; d < 10000; d++) {} } }

    vmm_init();
    LOG_I("kernel", "[I2] VMM initialized (MMU ready)");
    /* Debug checkpoint after VMM */
    { const char *m = "[PKB] VMM done\r\n"; for (const char *p = m; *p; p++) { __asm__ volatile("outb %0, %1" : : "a"((unsigned char)*p), "Nd"(0x3F8)); for (volatile int d = 0; d < 10000; d++) {} } }

    /* I4: Initialize Interrupt Descriptor Table (IDT) */
    idt_init();
    LOG_I("kernel", "[I4] IDT initialized");
    /* Debug checkpoint after IDT */
    { const char *m = "[PKB] IDT done\r\n"; for (const char *p = m; *p; p++) { __asm__ volatile("outb %0, %1" : : "a"((unsigned char)*p), "Nd"(0x3F8)); for (volatile int d = 0; d < 10000; d++) {} } }

    /* Initialize PIC (required before IDT is useful for hardware interrupts) */
    pic_init();
    LOG_I("kernel", "[I4] PIC initialized");

    /* I5: Initialize Global Descriptor Table with TSS BEFORE enabling interrupts!
     * 64-bit long mode requires a valid TSS for interrupt handling (iretq).
     * The boot GDT in boot.asm has no TSS entry. */
    gdt_init();
    LOG_I("kernel", "[I5] GDT initialized (with TSS)");
    /* Debug checkpoint after GDT */
    { const char *m = "[PKB] GDT done\r\n"; for (const char *p = m; *p; p++) { __asm__ volatile("outb %0, %1" : : "a"((unsigned char)*p), "Nd"(0x3F8)); for (volatile int d = 0; d < 10000; d++) {} } }

    /* Initialize PIT timer at 100Hz (safe now that GDT/TSS are loaded) */
    pit_init(100);
    idt_register_handler(32, timer_handler);
    pic_unmask_irq(0);
    LOG_I("kernel", "[I4] PIT timer at 100Hz");

    /* I6: Initialize System Call Interface */
    syscall_init();
    LOG_I("kernel", "[I6] System call interface initialized");
}

/* ============================================================
 * Phase J: 自检设备信息 (Device Self-Check)
 * J1: 扫描PCI总线设备
 * J2: 初始化CPU核心
 * J3: 初始化内存控制器
 * J4: 初始化存储设备驱动
 * J5: 初始化输入设备驱动(键盘/鼠标)
 * J6: 初始化显示设备驱动(VGA/Framebuffer)
 * J7: 检测并初始化网络设备
 * J8: 汇总设备信息到系统设备树
 * ============================================================ */

static void phase_device_selfcheck(void) {
    /* Initialize device tree first (J8 preparation) */
    devtree_init();

    /* J1: Scan PCI bus */
    pci_init();
    LOG_I("kernel", "[J1] PCI bus enumerated (%d devices)", pci_get_device_count());

    /* Register PCI devices into device tree */
    for (int i = 0; i < pci_get_device_count(); i++) {
        pci_device_t *d = pci_get_device(i);
        if (!d) continue;
        char name[DEV_NAME_MAX];
        /* Build a descriptive name from PCI class */
        const char *class_name = "pci-dev";
        if (d->class_code == 0x01) class_name = "ata-controller";
        else if (d->class_code == 0x02) class_name = "net-controller";
        else if (d->class_code == 0x03) class_name = "vga-controller";
        else if (d->class_code == 0x06) class_name = "pci-bridge";
        else if (d->class_code == 0x0C && d->subclass == 0x03) class_name = "usb-controller";
        snprintf(name, DEV_NAME_MAX, "%s-%02x:%02x.%x", class_name, (uint32_t)d->bus, (uint32_t)d->dev, (uint32_t)d->func);
        devtree_register(name, DEV_TYPE_PCI, DEV_STATUS_OK, DEV_CRITICAL_NO,
                         NULL, d->vendor_id, d->device_id);
    }

    /* J2: Initialize CPU cores (single core for now, detect features) */
    {
        uint32_t eax, ebx, ecx, edx;
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
        char vendor[13];
        memcpy(vendor, &ebx, 4);
        memcpy(vendor + 4, &edx, 4);
        memcpy(vendor + 8, &ecx, 4);
        vendor[12] = '\0';
        devtree_register("cpu-bsp", DEV_TYPE_CPU, DEV_STATUS_OK, DEV_CRITICAL_YES,
                         vendor, 0, 0);
        LOG_I("kernel", "[J2] BSP CPU initialized (vendor: %s)", vendor);
    }

    /* J3: Initialize memory controller (already configured by hardware/firmware) */
    {
        uint64_t total_pages = pmm_total_count();
        char info[DEV_INFO_MAX];
        snprintf(info, DEV_INFO_MAX, "%u MB total", (uint32_t)(total_pages * 4 / 1024));
        devtree_register("mem-controller", DEV_TYPE_MEMORY, DEV_STATUS_OK, DEV_CRITICAL_YES,
                         info, 0, 0);
        LOG_I("kernel", "[J3] Memory controller ready (%s)", info);
    }

    /* J4: Initialize storage device drivers (ATA) */
    ata_init();
    {
        int ata_count = ata_get_device_count();
        if (ata_count > 0) {
            for (int i = 0; i < ata_count; i++) {
                ata_device_t *d = ata_get_device(i);
                if (!d || !d->present) continue;
                char name[DEV_NAME_MAX];
                char info[DEV_INFO_MAX];
                uint64_t total = d->lba48 ? d->sectors_48 : d->sectors_28;
                uint64_t mb = (total * 512) / (1024 * 1024);
                snprintf(name, DEV_NAME_MAX, "ata-disk%d", i);
                snprintf(info, DEV_INFO_MAX, "%s %uMB", d->model, (uint32_t)mb);
                devtree_register(name, DEV_TYPE_STORAGE, DEV_STATUS_OK, DEV_CRITICAL_YES,
                                 info, 0, 0);
            }
            LOG_I("kernel", "[J4] ATA driver initialized (%d devices)", ata_count);
        } else {
            devtree_register("ata-disk0", DEV_TYPE_STORAGE, DEV_STATUS_MISSING, DEV_CRITICAL_NO,
                             "No storage device found", 0, 0);
            LOG_W("kernel", "[J4] No ATA storage devices found");
        }
    }

    /* J5: Initialize input device drivers (keyboard/mouse) */
    keyboard_init();
    devtree_register("ps2-keyboard", DEV_TYPE_INPUT, DEV_STATUS_OK, DEV_CRITICAL_YES,
                     "PS/2 keyboard", 0, 0);
    LOG_I("kernel", "[J5] PS/2 keyboard driver loaded");

    if (graphical_mode) {
        mouse_init(saved_fb_info.width, saved_fb_info.height);
        devtree_register("ps2-mouse", DEV_TYPE_INPUT, DEV_STATUS_OK, DEV_CRITICAL_NO,
                         "PS/2 mouse", 0, 0);
        LOG_I("kernel", "[J5] PS/2 mouse driver loaded");
    } else {
        devtree_register("ps2-mouse", DEV_TYPE_INPUT, DEV_STATUS_DISABLED, DEV_CRITICAL_NO,
                         "No framebuffer for mouse", 0, 0);
    }

    /* J6: Initialize display device drivers (VGA/Framebuffer) */
    if (!graphical_mode) {
        vga_init();
        vga_set_color(VGA_WHITE, VGA_BLUE);
        vga_puts(" SpiritFoxOS Kernel v1.0.0 - x86_64 ");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        vga_puts("\n\n");
        devtree_register("vga-text", DEV_TYPE_DISPLAY, DEV_STATUS_OK, DEV_CRITICAL_YES,
                         "VGA text 80x25", 0, 0);
        LOG_I("kernel", "[J6] VGA text mode initialized");
    } else {
        devtree_register("framebuffer", DEV_TYPE_DISPLAY, DEV_STATUS_OK, DEV_CRITICAL_YES,
                         "Framebuffer graphical mode", 0, 0);
        LOG_I("kernel", "[J6] Framebuffer display available (GUI will init later)");
    }

    /* J7: Detect and initialize network devices */
    {
        /* Check PCI devices for network controllers (class 0x02) */
        pci_device_t net_devs[16];
        int net_count = pci_find_all_class(0x02, 0xFF, net_devs, 16);
        if (net_count > 0) {
            for (int i = 0; i < net_count; i++) {
                char name[DEV_NAME_MAX];
                snprintf(name, DEV_NAME_MAX, "net-%02x:%02x.%x",
                         net_devs[i].bus, net_devs[i].dev, net_devs[i].func);
                devtree_register(name, DEV_TYPE_NETWORK, DEV_STATUS_MISSING, DEV_CRITICAL_NO,
                                 "No driver available", net_devs[i].vendor_id, net_devs[i].device_id);
            }
            LOG_I("kernel", "[J7] Found %d network device(s), no driver loaded", net_count);
        } else {
            devtree_register("network", DEV_TYPE_NETWORK, DEV_STATUS_MISSING, DEV_CRITICAL_NO,
                             "No network device detected", 0, 0);
            LOG_I("kernel", "[J7] No network devices detected");
        }
    }

    /* Initialize xHCI USB controller (part of J1/J7 device detection) */
    {
        pci_device_t xhci_dev;
        if (pci_find_device(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                            PCI_PROG_IF_XHCI, &xhci_dev)) {
            LOG_I("kernel", "xHCI controller found at %02x:%02x.%x",
                  xhci_dev.bus, xhci_dev.dev, xhci_dev.func);
            if (xhci_init(&xhci_dev) == 0) {
                usb_init();
                devtree_register("xhci-usb", DEV_TYPE_USB, DEV_STATUS_OK, DEV_CRITICAL_NO,
                                 "xHCI controller", xhci_dev.vendor_id, xhci_dev.device_id);
                LOG_I("kernel", "xHCI + USB subsystem initialized");
            } else {
                devtree_register("xhci-usb", DEV_TYPE_USB, DEV_STATUS_FAILED, DEV_CRITICAL_NO,
                                 "xHCI init failed", xhci_dev.vendor_id, xhci_dev.device_id);
                LOG_W("kernel", "xHCI controller init failed");
            }
        } else {
            LOG_I("kernel", "No xHCI controller found");
        }
    }

    /* J8: Device tree summary */
    LOG_I("kernel", "[J8] Device tree: %d devices registered", devtree_get_count());

    /* Enable interrupts after all devices are initialized */
    sti();
    LOG_I("kernel", "Interrupts enabled");
}

/* ============================================================
 * Phase K: 设备自检结果检查
 * K: 所有关键设备自检通过?
 * 否 -> L: 输出设备错误信息到控制台
 *       M: 进入最小化安全模式
 * 是 -> N: 挂载根文件系统
 * ============================================================ */

static int phase_device_check(void) {
    if (!devtree_check_critical()) {
        /* K: NO - critical device check failed */
        LOG_E("kernel", "[K] Critical device self-check FAILED");

        /* L: Output device error information to console */
        devtree_print_errors();

        /* M: Enter minimal safe mode */
        LOG_W("kernel", "[M] Entering safe mode");
        safe_mode_run();
        return 0; /* Never returns in safe mode, but for completeness */
    }

    /* K: YES - all critical devices passed */
    LOG_I("kernel", "[K] All critical devices passed self-check");
    return 1;
}

/* ============================================================
 * Phase N-P: 文件系统和进程管理
 * N: 挂载根文件系统
 * O: 初始化进程管理系统
 * P: 创建第一个用户进程(init进程)
 * ============================================================ */

static void phase_filesystem_and_processes(void) {
    /* N: Mount root filesystem */
    sfs_init();
    if (sfs_is_formatted()) {
        LOG_I("kernel", "[N] Root filesystem mounted");
    } else {
        LOG_I("kernel", "[N] No filesystem available (will be initialized by init)");
    }

    /* O: Initialize process management system */
    scheduler_init();
    LOG_I("kernel", "[O] Process scheduler initialized");

    perm_init();
    perm_register_system_app("terminal", PERM_FILE_READ | PERM_FILE_WRITE |
                             PERM_FILE_EXEC | PERM_SYS_INFO | PERM_GUI_RENDER |
                             PERM_INPUT_READ | PERM_PROCESS_MGMT);
    perm_register_system_app("filemanager", PERM_FILE_READ | PERM_FILE_WRITE |
                             PERM_FILE_EXEC | PERM_SYS_INFO | PERM_GUI_RENDER |
                             PERM_INPUT_READ | PERM_DEV_ACCESS);
    perm_register_system_app("settings", PERM_SYS_INFO | PERM_GUI_RENDER |
                             PERM_INPUT_READ | PERM_DEV_ACCESS |
                             PERM_PROCESS_MGMT | PERM_MEMORY_MGMT);
    LOG_I("kernel", "[O] Permission manager initialized (3 system apps)");

    /* P: Create first user process (init) */
    /* For now, init runs in kernel context. When user mode is implemented,
     * this will become a true user-space process. */
    LOG_I("kernel", "[P] Init process ready");
}

/* ============================================================
 * Phase Q: 启动命令行界面 (Start CLI)
 * Q1: 初始化终端驱动
 * Q2: 启动shell解释器
 * Q3: 显示命令行提示符
 * R:  等待用户输入命令
 * S:  解析并执行用户命令
 * ============================================================ */

static void phase_start_cli(void) {
    if (graphical_mode) {
        /* Q1: Initialize terminal driver (GUI framebuffer terminal) */
        LOG_I("kernel", "[Q] Launching GUI (%ux%u %ubpp)",
              saved_fb_info.width, saved_fb_info.height,
              saved_fb_info.bpp);

        gui_init(saved_fb_info.address,
                 saved_fb_info.width,
                 saved_fb_info.height,
                 saved_fb_info.pitch,
                 saved_fb_info.bpp);

        /* Q2-Q3: Start shell interpreter in GUI terminal */
        scheduler_set_enabled(0);  /* Disable scheduler during GUI to prevent context switch */
        gui_run();

        /* If gui_run() returned (e.g. force-exit shortcut), switch to text shell */
        LOG_I("kernel", "GUI exited, switching to VGA text shell");
        vga_init();
        vga_set_color(VGA_WHITE, VGA_BLUE);
        vga_puts(" SpiritFoxOS Kernel v1.0.0 - x86_64 (Text Mode) ");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        vga_puts("\n\n");
        init_process();
    } else {
        /* Q1: Terminal driver already initialized (VGA text mode) */
        /* Q2-Q3: Start shell interpreter with prompt */
        LOG_I("kernel", "[Q] Starting text shell");
        init_process();
    }
}

/* ============================================================
 * Multiboot2 definitions for BIOS boot path
 * ============================================================ */
#define MB2_MAGIC 0x36D76289

/* Multiboot2 tag types */
#define MB2_TAG_END         0
#define MB2_TAG_CMDLINE     1
#define MB2_TAG_BOOT_LOADER_NAME 2
#define MB2_TAG_MODULE      3
#define MB2_TAG_FRAMEBUFFER 4
#define MB2_TAG_ACP_OLD     5  /* deprecated */
#define MB2_TAG_MMAP        6

/* Multiboot2 info header */
typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} mb2_info_t;

/* Generic Multiboot2 tag header */
typedef struct {
    uint32_t type;
    uint32_t size;
} mb2_tag_t;

/* Framebuffer tag (type 4) */
typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t fb_addr;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint8_t  fb_bpp;
    uint8_t  fb_type;
    uint8_t  reserved[2];
} mb2_fb_tag_t;

/* Memory map entry (within mmap tag) */
typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t zero;
} mb2_mmap_entry_t;

/* Memory map tag (type 6) */
typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* entries follow */
} mb2_mmap_tag_t;

/* ============================================================
 * Parse Multiboot2 info into unified bootinfo structure
 * Called in BIOS mode when bootinfo magic doesn't match.
 * ============================================================ */
static int parse_multiboot2(uint32_t mb2_magic, void *mb2_info,
                             bootinfo_t *out) {
    if (mb2_magic != MB2_MAGIC || !mb2_info) return -1;

    __builtin_memset(out, 0, sizeof(bootinfo_t));
    out->magic = BOOTINFO_MAGIC;

    mb2_info_t *info = (mb2_info_t *)mb2_info;
    uint8_t *tags = (uint8_t *)info + sizeof(mb2_info_t);
    uint8_t *end = (uint8_t *)info + info->total_size;

    while ((uint8_t*)tags + sizeof(mb2_tag_t) <= end) {
        mb2_tag_t *tag = (mb2_tag_t *)tags;
        if (tag->type == MB2_TAG_END) break;
        if (tag->size < sizeof(mb2_tag_t)) break;

        switch (tag->type) {
            case MB2_TAG_FRAMEBUFFER: {
                /* Read fields at known offsets from tag start (no struct packing issues):
                 * offset 8:  u64 framebuffer_addr
                 * offset 16: u32 pitch
                 * offset 20: u32 width
                 * offset 24: u32 height
                 * offset 28: u8  bpp
                 * offset 29: u8  type (0=indexed, 1=RGB, 2=text)
                 */
                uint8_t *raw = (uint8_t *)tag;
                uint64_t fb_addr = *(uint64_t *)(raw + 8);
                uint32_t fb_pitch = *(uint32_t *)(raw + 16);
                uint32_t fb_width = *(uint32_t *)(raw + 20);
                uint32_t fb_height = *(uint32_t *)(raw + 24);
                uint8_t fb_bpp = *(uint8_t *)(raw + 28);
                uint8_t fb_type_raw = *(uint8_t *)(raw + 29);

                out->framebuffer.address = fb_addr;
                out->framebuffer.pitch = fb_pitch;
                out->framebuffer.width = fb_width;
                out->framebuffer.height = fb_height;
                out->framebuffer.bpp = fb_bpp;
                out->framebuffer.type = (fb_type_raw == 1) ? BOOTINFO_FB_RGB : BOOTINFO_FB_TEXT;

                /* If we have a valid-looking framebuffer address, force RGB mode */
                if (fb_addr != 0 && fb_width > 0 && fb_height > 0) {
                    out->framebuffer.type = BOOTINFO_FB_RGB;
                }
                break;
            }
            case MB2_TAG_MMAP: {
                mb2_mmap_tag_t *mmap_tag = (mb2_mmap_tag_t *)tag;
                out->mmap_entry_size = mmap_tag->entry_size ?
                                       mmap_tag->entry_size : sizeof(mb2_mmap_entry_t);
                /* First entry starts right after the mmap tag header fields */
                out->mmap_addr = (uint64_t)(uintptr_t)(tags + sizeof(mb2_mmap_tag_t));
                /* Count entries */
                uint32_t data_size = tag->size - sizeof(mb2_mmap_tag_t);
                out->mmap_entry_count = data_size / out->mmap_entry_size;
                break;
            }
            default:
                break;
        }

        /* Advance to next tag (8-byte aligned) */
        tags += (tag->size + 7) & ~7;
    }

    /* Set kernel bounds from linker symbols */
    out->kernel_start = (uint64_t)(uintptr_t)_start;
    out->kernel_end = (uint64_t)(uintptr_t)_end;

    return 0;
}

/* Static buffer for converting Multiboot2 mmap entries to EFI format.
 * Max 128 entries should be enough for typical systems. */
#define MAX_MMAP_ENTRIES 128
static EFI_MEMORY_DESCRIPTOR mb2_to_efi_buf[MAX_MMAP_ENTRIES];

/*
 * Convert Multiboot2 memory map entries to EFI_MEMORY_DESCRIPTOR format
 * so that pmm_init() can process them without changes.
 */
static int convert_mmap_mb2_to_efi(bootinfo_t *bootinfo) {
    if (!bootinfo->mmap_addr || !bootinfo->mmap_entry_count) return -1;
    if (bootinfo->mmap_entry_count > MAX_MMAP_ENTRIES) return -1;

    uint8_t *entries = (uint8_t *)(uintptr_t)bootinfo->mmap_addr;
    uint64_t entry_size = bootinfo->mmap_entry_size;
    uint64_t count = bootinfo->mmap_entry_count;

    for (uint64_t i = 0; i < count && i < MAX_MMAP_ENTRIES; i++) {
        mb2_mmap_entry_t *mb2_ent =
            (mb2_mmap_entry_t *)(entries + i * entry_size);
        EFI_MEMORY_DESCRIPTOR *efi_desc = &mb2_to_efi_buf[i];

        /* Map MB2 types to EFI types */
        switch (mb2_ent->type) {
            case 1: efi_desc->type = 7;  break;  /* Available → Conventional */
            case 2: efi_desc->type = 11; break;  /* Reserved → MMIO */
            case 3: efi_desc->type = 9;  break;  /* ACPI → ACPI Reclaimable */
            case 4: efi_desc->type = 10; break;  /* NVS → ACPI NVS */
            case 5: efi_desc->type = 15; break;  /* Bad → Unusable */
            default: efi_desc->type = 11; break;  /* Unknown → Reserved */
        }

        efi_desc->physical_start = mb2_ent->base_addr;
        efi_desc->virtual_start = 0;
        efi_desc->number_of_pages = mb2_ent->length / PAGE_SIZE;
        efi_desc->attribute = 0;
    }

    /* Point bootinfo to the converted buffer */
    bootinfo->mmap_addr = (uint64_t)(uintptr_t)mb2_to_efi_buf;
    bootinfo->mmap_entry_size = sizeof(EFI_MEMORY_DESCRIPTOR);

    return 0;
}

/* ============================================================
 * Main kernel entry point
 * BIOS mode:  rdi=MB2_MAGIC(0x36D76289), rsi=multiboot2_info_ptr
 * UEFI mode: rdi=bootinfo_t*, rsi=unused
 * ============================================================ */

void kernel_main(bootinfo_t *bootinfo_param) {
    /* Debug: direct COM1 output to verify we reached kernel_main */
    {
        const char *msg = "[KM] kernel_main entered\r\n";
        for (const char *p = msg; *p; p++) {
            __asm__ volatile("outb %0, %1" : : "a"((unsigned char)*p), "Nd"(0x3F8));
            for (volatile int d = 0; d < 10000; d++);
        }
    }

    /*
     * Detect boot mode and prepare unified bootinfo structure.
     * In BIOS mode, bootinfo_param is actually the Multiboot2 magic number
     * (passed in RDI from boot.asm), not a valid pointer.
     */
    static bootinfo_t bootinfo;

    if (bootinfo_param && bootinfo_param->magic == BOOTINFO_MAGIC) {
        /* UEFI mode: bootinfo is valid, copy it */
        __builtin_memcpy(&bootinfo, bootinfo_param, sizeof(bootinfo_t));
        { const char *m = "[KM] UEFI mode detected\r\n"; for (const char *p = m; *p; p++) { __asm__ volatile("outb %0, %1" : : "a"((unsigned char)*p), "Nd"(0x3F8)); for (volatile int d = 0; d < 10000; d++) {} } }
    } else {
        /* BIOS mode: parse Multiboot2 info from RDI/RSI registers */
        uint32_t mb2_magic;
        void *mb2_info;
        __asm__ volatile("mov %%edi, %0" : "=r"(mb2_magic));
        __asm__ volatile("mov %%rsi, %0" : "=r"(mb2_info));

        { const char *m = "[KM] BIOS mode detected, parsing MB2\r\n"; for (const char *p = m; *p; p++) { __asm__ volatile("outb %0, %1" : : "a"((unsigned char)*p), "Nd"(0x3F8)); for (volatile int d = 0; d < 10000; d++) {} } }

        if (parse_multiboot2(mb2_magic, mb2_info, &bootinfo) != 0) {
            const char *err = "[ERR] Failed to parse Multiboot2 info!\r\n";
            for (const char *p = err; *p; p++) {
                __asm__ volatile("outb %0, %1" : : "a"((unsigned char)*p), "Nd"(0x3F8));
                for (volatile int d = 0; d < 10000; d++);
            }
            hlt();
        }
        { const char *m = "[KM] MB2 parsed OK\r\n"; for (const char *p = m; *p; p++) { __asm__ volatile("outb %0, %1" : : "a"((unsigned char)*p), "Nd"(0x3F8)); for (volatile int d = 0; d < 10000; d++) {} } }

        /* Convert MB2 memory map entries to EFI format for PMM */
        if (convert_mmap_mb2_to_efi(&bootinfo) != 0) {
            const char *err = "[ERR] Failed to convert MB2 mmap!\r\n";
            for (const char *p = err; *p; p++) {
                __asm__ volatile("outb %0, %1" : : "a"((unsigned char)*p), "Nd"(0x3F8));
                for (volatile int d = 0; d < 10000; d++);
            }
            hlt();
        }
        { const char *m = "[KM] MB2 mmap converted\r\n"; for (const char *p = m; *p; p++) { __asm__ volatile("outb %0, %1" : : "a"((unsigned char)*p), "Nd"(0x3F8)); for (volatile int d = 0; d < 10000; d++) {} } }
    }

    /* Phase I: 系统内核启动 */
    phase_kernel_boot(&bootinfo);

    /* Phase J: 自检设备信息 */
    phase_device_selfcheck();

    /* Phase K: 设备自检结果检查 (may enter safe mode) */
    if (!phase_device_check()) {
        /* Safe mode took over, should not reach here */
        while (1) hlt();
    }

    /* Phase N-P: 文件系统和进程管理 */
    phase_filesystem_and_processes();

    /* Phase Q: 启动命令行界面 */
    phase_start_cli();

    while (1) hlt();
}
