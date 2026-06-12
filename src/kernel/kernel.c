/* SpiritFoxOS - 内核主入口
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

/* 内核起始/结束符号（来自链接脚本） */
extern char _start[];
extern char _end[];

/* 定时器tick计数器 */
volatile uint64_t timer_ticks = 0;

/* 标志：是否处于图形模式？ */
static int graphical_mode = 0;

/* Bootinfo帧缓冲区信息（保存供后续GUI初始化使用） */
static bootinfo_fb_t saved_fb_info;

/* 定时器中断处理函数 */
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
    /* 调试：尽快直接输出到COM1串口 */
    {
        const char *msg = "[PKB] phase_kernel_boot entered\r\n";
        for (const char *p = msg; *p; p++) {
            for (volatile int d = 0; d < 10000; d++);
        }
    }
    /* 验证bootinfo魔数 */
    if (bootinfo->magic != BOOTINFO_MAGIC) {
        /* 无有效启动信息则停机 */
        hlt();
    }

    /* 早期调试输出 */
    serial_init(COM1);
    log_init();
    LOG_I("kernel", "SpiritFoxOS starting (UEFI mode)...");

    /*
     * 解析bootinfo获取帧缓冲区和内存映射信息。
     * 来自UEFI的内存映射使用EFI_MEMORY_DESCRIPTOR条目。
     * 我们将其转换为与PMM兼容的格式。
     */

    /* 提取帧缓冲区信息 */
    if (bootinfo->framebuffer.type == BOOTINFO_FB_RGB) {
        graphical_mode = 1;
        __builtin_memcpy(&saved_fb_info, &bootinfo->framebuffer, sizeof(bootinfo_fb_t));
    }

    /*
     * 内存映射：bootinfo包含原始EFI内存描述符。
     * 将EFI内存类型转换为PMM兼容类型：
     *   EFI_CONVENTIONAL_MEMORY (7) -> AVAILABLE
     *   EFI_UNUSABLE_MEMORY (8)  -> UNUSABLE
     *   EFI_ACPI_RECLAIM_MEMORY (9) -> ACPI_RECLAIMABLE
     *   EFI_ACPI_MEMORY_NVS (10)   -> NVS
     *   其他所有类型               -> RESERVED
     *
     * 我们直接传递EFI内存映射；pmm_init_efi()负责转换。
     */

    /* I2: 初始化内存管理单元（PMM + VMM） */
    if (!bootinfo->mmap_addr || !bootinfo->mmap_entry_count) {
        LOG_F("kernel", "No memory map from bootloader!");
        hlt();
    }

    uint64_t kernel_start = bootinfo->kernel_start;
    uint64_t kernel_end = bootinfo->kernel_end;
    pmm_init(bootinfo, kernel_start, kernel_end);
    LOG_I("kernel", "[I2] PMM initialized (kernel %p-%p)", kernel_start, kernel_end);
    /* PMM之后的调试检查点 */

    vmm_init();
    LOG_I("kernel", "[I2] VMM initialized (MMU ready)");
    /* VMM之后的调试检查点 */

    /* I4: 初始化中断描述符表（IDT） */
    idt_init();
    LOG_I("kernel", "[I4] IDT initialized");
    /* IDT之后的调试检查点 */

    /* 初始化PIC（IDT对硬件中断有效前必须先初始化PIC） */
    pic_init();
    LOG_I("kernel", "[I4] PIC initialized");

    /* I5: 在启用中断之前初始化带TSS的全局描述符表！
     * 64位长模式需要有效的TSS用于中断处理（iretq）。
     * boot.asm中的启动GDT没有TSS条目。 */
    gdt_init();
    LOG_I("kernel", "[I5] GDT initialized (with TSS)");
    /* GDT之后的调试检查点 */

    /* 初始化PIT定时器为100Hz（GDT/TSS已加载，现在安全） */
    pit_init(100);
    idt_register_handler(32, timer_handler);
    pic_unmask_irq(0);
    LOG_I("kernel", "[I4] PIT timer at 100Hz");

    /* I6: 初始化系统调用接口 */
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
    /* 首先初始化设备树（为J8做准备） */
    devtree_init();

    /* J1: 扫描PCI总线 */
    pci_init();
    LOG_I("kernel", "[J1] PCI bus enumerated (%d devices)", pci_get_device_count());

    /* 将PCI设备注册到设备树 */
    for (int i = 0; i < pci_get_device_count(); i++) {
        pci_device_t *d = pci_get_device(i);
        if (!d) continue;
        char name[DEV_NAME_MAX];
        /* 根据PCI类代码构建描述性名称 */
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

    /* J2: 初始化CPU核心（目前单核，检测特性） */
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

    /* J3: 初始化内存控制器（已由硬件/固件配置） */
    {
        uint64_t total_pages = pmm_total_count();
        char info[DEV_INFO_MAX];
        snprintf(info, DEV_INFO_MAX, "%u MB total", (uint32_t)(total_pages * 4 / 1024));
        devtree_register("mem-controller", DEV_TYPE_MEMORY, DEV_STATUS_OK, DEV_CRITICAL_YES,
                         info, 0, 0);
        LOG_I("kernel", "[J3] Memory controller ready (%s)", info);
    }

    /* J4: 初始化存储设备驱动（ATA） */
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

    /* J5: 初始化输入设备驱动（键盘/鼠标） */
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

    /* J6: 初始化显示设备驱动（VGA/帧缓冲区） */
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

    /* J7: 检测并初始化网络设备 */
    {
        /* 检查PCI设备中的网络控制器（类代码0x02） */
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

    /* 初始化xHCI USB控制器（属于J1/J7设备检测的一部分） */
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

    /* J8: 设备树汇总 */
    LOG_I("kernel", "[J8] Device tree: %d devices registered", devtree_get_count());

    /* 所有设备初始化完成后启用中断 */
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
        /* K: 否 - 关键设备自检失败 */
        LOG_E("kernel", "[K] Critical device self-check FAILED");

        /* L: 将设备错误信息输出到控制台 */
        devtree_print_errors();

        /* M: 进入最小化安全模式 */
        LOG_W("kernel", "[M] Entering safe mode");
        safe_mode_run();
        return 0; /* 安全模式不会返回，但为了完整性 */
    }

    /* K: 是 - 所有关键设备自检通过 */
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
    /* N: 挂载根文件系统 */
    sfs_init();
    if (sfs_is_formatted()) {
        LOG_I("kernel", "[N] Root filesystem mounted");
    } else {
        LOG_I("kernel", "[N] No filesystem available (will be initialized by init)");
    }

    /* O: 初始化进程管理系统 */
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

    /* P: 创建第一个用户进程（init） */
    /* 目前init在内核上下文中运行。当用户模式实现后，
     * 这将变为真正的用户空间进程。 */
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
        /* Q1: 初始化终端驱动（GUI帧缓冲区终端） */
        LOG_I("kernel", "[Q] Launching GUI (%ux%u %ubpp)",
              saved_fb_info.width, saved_fb_info.height,
              saved_fb_info.bpp);

        gui_init(saved_fb_info.address,
                 saved_fb_info.width,
                 saved_fb_info.height,
                 saved_fb_info.pitch,
                 saved_fb_info.bpp);

        /* Q2-Q3: 在GUI终端中启动shell解释器 */
        scheduler_set_enabled(0);  /* GUI期间禁用调度器以防止上下文切换 */
        gui_run();

        /* 如果gui_run()返回（例如强制退出快捷键），切换到文本shell */
        LOG_I("kernel", "GUI exited, switching to VGA text shell");
        vga_init();
        vga_set_color(VGA_WHITE, VGA_BLUE);
        vga_puts(" SpiritFoxOS Kernel v1.0.0 - x86_64 (Text Mode) ");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        vga_puts("\n\n");
        init_process();
    } else {
        /* Q1: 终端驱动已初始化（VGA文本模式） */
        /* Q2-Q3: 启动带提示符的shell解释器 */
        LOG_I("kernel", "[Q] Starting text shell");
        init_process();
    }
}

/* ============================================================
 * Multiboot2 definitions for BIOS boot path
 * ============================================================ */
#define MB2_MAGIC 0x36D76289

/* Multiboot2标签类型（来自GNU multiboot2.h） */
#define MB2_TAG_END         0
#define MB2_TAG_CMDLINE     1
#define MB2_TAG_BOOT_LOADER_NAME 2
#define MB2_TAG_MODULE      3
#define MB2_TAG_BASIC_MEMINFO 4
#define MB2_TAG_BOOTDEV     5
#define MB2_TAG_MMAP        6
#define MB2_TAG_VBE         7
#define MB2_TAG_FRAMEBUFFER 8

/* Multiboot2信息头 */
typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} mb2_info_t;

/* 通用Multiboot2标签头 */
typedef struct {
    uint32_t type;
    uint32_t size;
} mb2_tag_t;

/* 帧缓冲区标签（类型8） */
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

/* 内存映射条目（在mmap标签内） */
typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t zero;
} mb2_mmap_entry_t;

/* 内存映射标签（类型6） */
typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* entries follow */
} mb2_mmap_tag_t;

/* ============================================================
 * 将Multiboot2信息解析到统一的bootinfo结构中
 * 在BIOS模式下当bootinfo魔数不匹配时调用。
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
                /* 使用紧缩结构体转换以可靠地访问字段 */
                typedef struct __attribute__((packed)) {
                    uint32_t type;
                    uint32_t size;
                    uint64_t fb_addr;
                    uint32_t fb_pitch;
                    uint32_t fb_width;
                    uint32_t fb_height;
                    uint8_t  fb_bpp;
                    uint8_t  fb_type;
                    uint8_t  reserved[2];
                } fb_tag_raw_t;
                fb_tag_raw_t *fb = (fb_tag_raw_t *)tag;

                out->framebuffer.address = fb->fb_addr;
                out->framebuffer.pitch = fb->fb_pitch;
                out->framebuffer.width = fb->fb_width;
                out->framebuffer.height = fb->fb_height;
                out->framebuffer.bpp = fb->fb_bpp;
                out->framebuffer.type = (fb->fb_type == 1) ? BOOTINFO_FB_RGB : BOOTINFO_FB_TEXT;

                /* 如果有有效尺寸则强制设为RGB */
                if (fb->fb_addr != 0 && fb->fb_width > 0 && fb->fb_height > 0) {
                    out->framebuffer.type = BOOTINFO_FB_RGB;
                }
                break;
            }
            case MB2_TAG_MMAP: {
                mb2_mmap_tag_t *mmap_tag = (mb2_mmap_tag_t *)tag;
                out->mmap_entry_size = mmap_tag->entry_size ?
                                       mmap_tag->entry_size : sizeof(mb2_mmap_entry_t);
                /* 第一个条目紧跟在mmap标签头字段之后 */
                out->mmap_addr = (uint64_t)(uintptr_t)(tags + sizeof(mb2_mmap_tag_t));
                /* 统计条目数 */
                uint32_t data_size = tag->size - sizeof(mb2_mmap_tag_t);
                out->mmap_entry_count = data_size / out->mmap_entry_size;
                break;
            }
            default:
                break;
        }

        /* 前进到下一个标签（8字节对齐） */
        tags += (tag->size + 7) & ~7;
    }

    /* 从链接器符号设置内核边界 */
    out->kernel_start = (uint64_t)(uintptr_t)_start;
    out->kernel_end = (uint64_t)(uintptr_t)_end;

    return 0;
}

/* 用于将Multiboot2内存映射条目转换为EFI格式的静态缓冲区。
 * 最多128个条目对典型系统来说应该足够了。 */
#define MAX_MMAP_ENTRIES 128
static EFI_MEMORY_DESCRIPTOR mb2_to_efi_buf[MAX_MMAP_ENTRIES];

/*
 * 将Multiboot2内存映射条目转换为EFI_MEMORY_DESCRIPTOR格式
 * 以便pmm_init()无需修改即可处理。
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

        /* 将MB2类型映射到EFI类型 */
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

    /* 将bootinfo指向转换后的缓冲区 */
    bootinfo->mmap_addr = (uint64_t)(uintptr_t)mb2_to_efi_buf;
    bootinfo->mmap_entry_size = sizeof(EFI_MEMORY_DESCRIPTOR);

    return 0;
}

/* ============================================================
 * 内核主入口点
 * BIOS模式:  rdi=MB2_MAGIC(0x36D76289), rsi=multiboot2_info_ptr
 * UEFI模式: rdi=bootinfo_t*, rsi=未使用
 * ============================================================ */

void __attribute__((used)) kernel_main(bootinfo_t *bootinfo_param) {
    /* 调试：直接输出到COM1以确认已到达kernel_main */
    {
        const char *msg = "[KM] kernel_main entered\r\n";
        for (const char *p = msg; *p; p++) {
            for (volatile int d = 0; d < 10000; d++);
        }
    }

    /*
     * 检测启动模式并准备统一的bootinfo结构。
     * 在BIOS模式下，bootinfo_param实际上是Multiboot2魔数
     *（从boot.asm通过RDI传入），不是有效指针。
     */
    static bootinfo_t bootinfo;

    if (bootinfo_param && bootinfo_param->magic == BOOTINFO_MAGIC) {
        /* UEFI模式：bootinfo有效，复制它 */
        __builtin_memcpy(&bootinfo, bootinfo_param, sizeof(bootinfo_t));
    } else {
        /* BIOS模式：从RDI/RSI寄存器解析Multiboot2信息 */
        uint32_t mb2_magic;
        void *mb2_info;
        __asm__ volatile("mov %%edi, %0" : "=r"(mb2_magic));
        __asm__ volatile("mov %%rsi, %0" : "=r"(mb2_info));


        if (parse_multiboot2(mb2_magic, mb2_info, &bootinfo) != 0) {
            const char *err = "[ERR] Failed to parse Multiboot2 info!\r\n";
            for (const char *p = err; *p; p++) {
                for (volatile int d = 0; d < 10000; d++);
            }
            hlt();
        }

        /* 将MB2内存映射条目转换为EFI格式供PMM使用 */
        if (convert_mmap_mb2_to_efi(&bootinfo) != 0) {
            const char *err = "[ERR] Failed to convert MB2 mmap!\r\n";
            for (const char *p = err; *p; p++) {
                for (volatile int d = 0; d < 10000; d++);
            }
            hlt();
        }
    }

    /* Phase I: 系统内核启动 */
    phase_kernel_boot(&bootinfo);

    /* Phase J: 自检设备信息 */
    phase_device_selfcheck();

    /* Phase K: 设备自检结果检查 (may enter safe mode) */
    if (!phase_device_check()) {
        /* 安全模式已接管，不应到达此处 */
        while (1) hlt();
    }

    /* Phase N-P: 文件系统和进程管理 */
    phase_filesystem_and_processes();

    /* Phase Q: 启动命令行界面 */
    phase_start_cli();

    while (1) hlt();
}
