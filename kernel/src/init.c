/*
 * SpiritFoxOS Kernel Subsystem Initialization
 */

#include "boot.h"
#include "gdt.h"
#include "idt.h"
#include "memory.h"
#include "mmu.h"
#include "vga.h"
#include "keyboard.h"
#include "mouse.h"
#include "acpi.h"
#include "apic.h"
#include "pci.h"
#include "timer.h"
#include "hal.h"
#include "blkdev.h"
#include "ahci.h"
#include "ramdisk.h"
#include "terminal.h"
#include "vfs.h"
#include "memfs.h"
#include "devfs.h"
#include "fat32.h"
#include "string.h"
#include "kmalloc.h"
#include "process.h"
#include "syscall.h"
#include "registry.h"
#include "pkgmgr.h"
#include "sandbox.h"
#include "procfs.h"
#include "net.h"
#include "net_socket.h"
#include "net_tcp.h"
#include "net_icmp.h"
#include "smp.h"
#include "slab.h"
#include "rtl8139.h"
#include "serial.h"
#include "autorun.h"
#include "drm_gem.h"
#include "chardev.h"

/* 前向声明 input_init()，避免引入 input.h —— 后者会与 keyboard.h
 * 中的 KEY_UP/KEY_DOWN/KEY_TAB 等扩展键码宏产生重定义冲突。
 * keyboard.h / mouse.h 同样以前向声明方式引用 struct input_dev。 */
void input_init(void);

extern BootInfo *g_boot_info;

void init_core(BootInfo *boot_info)
{
    serial_init();
    serial_puts("[SpiritFoxOS] kernel_main reached!\n");

    if (!boot_info) {
        serial_puts("[SpiritFoxOS] ERROR: boot_info is NULL!\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    serial_puts("[SpiritFoxOS] Checking magic...\n");
    if (boot_info->magic != BOOT_INFO_MAGIC) {
        serial_puts("[SpiritFoxOS] ERROR: magic mismatch!\n");
        volatile uint16_t* vga = (volatile uint16_t*)0xB8000;
        const char* err = "BOOT INFO INVALID";
        for (int i = 0; err[i]; i++) {
            vga[i] = (uint16_t)err[i] | 0x4F00;
        }
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    serial_puts("[SpiritFoxOS] Magic OK, initializing...\n");

    serial_puts("[SpiritFoxOS] gdt_init...\n");
    gdt_init();

    serial_puts("[SpiritFoxOS] idt_init...\n");
    idt_init();

    serial_puts("[SpiritFoxOS] memory_init...\n");
    {
        extern char __bss_end[];
        uintptr_t kernel_end = (uintptr_t)__bss_end;
        kernel_end = (kernel_end + 0x3000) & ~(uintptr_t)0xFFF;
        memory_init(boot_info, kernel_end);
    }

    serial_puts("[SpiritFoxOS] mmu_init...\n");
    mmu_init();

    /* UEFI sets CR0.WP (Write Protect) which prevents kernel writes
     * to read-only pages. We need to clear it so hal_ensure_mapped
     * can modify UEFI's page tables. */
    if (boot_info->boot_type == BOOT_TYPE_UEFI) {
        uint64_t cr0;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~(1ULL << 16);  /* Clear WP bit */
        __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
        serial_puts("[SpiritFoxOS] Cleared CR0.WP for UEFI boot\n");
    }

    /* 启用 SSE/SSE2 支持（用户进程需要 XMM 指令） */
    {
        uint64_t cr0, cr4;
        /* CR0: 清除 EM(bit2), 设置 MP(bit1) */
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~(1ULL << 2);   /* Clear EM: 不模拟x87 */
        cr0 |= (1ULL << 1);    /* Set MP: WAIT/FWAIT触发#NM */
        __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
        /* CR4: 设置 OSFXSR(bit9) 和 OSXMMEXCPT(bit10) */
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 9);    /* OSFXSR: 操作系统支持FXSAVE/FXSTOR */
        cr4 |= (1ULL << 10);   /* OSXMMEXCPT: 操作系统支持XMM异常 */
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    }

    serial_puts("[SpiritFoxOS] pci_init...\n");
    pci_init();

    serial_puts("[SpiritFoxOS] vga_init...\n");
    vga_init(boot_info);

    printf("[SpiritFoxOS] Initializing hardware...\n");
}

void init_hardware(void)
{
    serial_puts("[SpiritFoxOS] acpi_init...\n");
    acpi_init();
    printf("[ACPI] Initialized\n");

    serial_puts("[SpiritFoxOS] apic_init...\n");
    apic_init();
    printf("[APIC] Initialized\n");

    serial_puts("[SpiritFoxOS] smp_enumerate_cpus_only...\n");
    smp_enumerate_cpus_only();

    serial_puts("[SpiritFoxOS] timer_init...\n");
    timer_init();

    serial_puts("[SpiritFoxOS] keyboard_init...\n");
    keyboard_init();

    serial_puts("[SpiritFoxOS] mouse_init...\n");
    mouse_init();
}

void init_storage(void)
{
    serial_puts("[SpiritFoxOS] blkdev_init...\n");
    blkdev_init();

    serial_puts("[SpiritFoxOS] ahci_init...\n");
    ahci_init();

    serial_puts("[SpiritFoxOS] ramdisk_init...\n");
    ramdisk_init();

    /* 注册 initrd 为 ramdisk 块设备（如果存在） */
    if (g_boot_info->initrd_addr != 0 && g_boot_info->initrd_size != 0) {
        void *initrd_ptr = (void *)(uintptr_t)g_boot_info->initrd_addr;
        printf("[SpiritFoxOS] Registering initrd at 0x%llx, size=%llu bytes\n",
               (unsigned long long)g_boot_info->initrd_addr,
               (unsigned long long)g_boot_info->initrd_size);
        int initrd_dev = ramdisk_create_from_buffer(initrd_ptr, g_boot_info->initrd_size);
        if (initrd_dev >= 0) {
            printf("[SpiritFoxOS] initrd registered as blkdev %d\n", initrd_dev);
        }
    } else {
        /* 无 initrd，创建默认空 ramdisk */
        ramdisk_create(RAMDISK_DEFAULT_SECTORS);
    }

    serial_puts("[SpiritFoxOS] terminal_init...\n");
    terminal_init();
}

void init_filesystem(void)
{
    serial_puts("[SpiritFoxOS] vfs_init...\n");
    vfs_init();
    memfs_init();
    devfs_init();
    fat32_init();

    serial_puts("[SpiritFoxOS] mounting root...\n");
    vfs_mount(NULL, "/", "memfs", 0, NULL);

    vfs_mkdir("/dev", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mount(NULL, "/dev", "devfs", 0, NULL);

    printf("[VFS] Root filesystem mounted\n");

    /* Mount FAT32 filesystem at /mnt */
    {
        extern blkdev_t *blkdev_get(uint8_t dev_id);
        vfs_mkdir("/mnt", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);

        int mounted = 0;
        for (int i = 0; i < 16 && !mounted; i++) {
            blkdev_t *dev = blkdev_get((uint8_t)i);
            if (!dev || !dev->in_use)
                continue;

            char opts[32];
            char num_buf[12];
            utoa((unsigned int)i, num_buf, 10);
            strcpy(opts, "blkdev=");
            strcat(opts, num_buf);

            printf("[FAT32] Trying blkdev=%d (type=%d)...\n", i, dev->type);
            int mret = vfs_mount(NULL, "/mnt", "fat32", 0, opts);
            if (mret == 0) {
                printf("[FAT32] Mounted at /mnt (blkdev=%d)\n", i);
                mounted = 1;
            } else {
                printf("[FAT32] blkdev=%d mount failed (err=%d)\n", i, mret);
            }
        }

        if (!mounted) {
            printf("[FAT32] No suitable block device found for FAT32 mount\n");
        }
    }
}

/* GS/physical memory are ready; no process, disk or VFS dependency. */
void init_allocators(void)
{
    serial_puts("[SpiritFoxOS] kmalloc_init...\n");
    kmalloc_init();

    serial_puts("[SpiritFoxOS] slab_init...\n");
    slab_init();
    printf("[SLAB] Slab allocator initialized\n");
}

void init_services(void)
{
    serial_puts("[SpiritFoxOS] process_init...\n");
    process_init();

    serial_puts("[SpiritFoxOS] syscall_init...\n");
    syscall_init();

    serial_puts("[SpiritFoxOS] smp_start_aps...\n");
    smp_start_aps();

    serial_puts("[SpiritFoxOS] registry_init...\n");
    registry_init();

    serial_puts("[SpiritFoxOS] pkgmgr_init...\n");
    pkgmgr_init();

    serial_puts("[SpiritFoxOS] sandbox_init...\n");
    sandbox_init();

    serial_puts("[SpiritFoxOS] net_init...\n");
    net_init();
    printf("[NET] Network stack initialized\n");
}

void init_fs_hierarchy(void)
{
    vfs_mkdir("/bin", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/sbin", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/etc", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/usr", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/usr/bin", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/usr/lib", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/usr/share", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/var", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/var/log", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/opt", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/opt/sfk", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/home", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/root", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/tmp", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/run", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/run/user", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/run/user/0", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
    vfs_mkdir("/proc", VFS_S_IRUSR | VFS_S_IXUSR);
    vfs_mkdir("/sys", VFS_S_IRUSR | VFS_S_IXUSR);
    printf("[FHS] Standard Linux directory structure created\n");

    /* Create default autorun.cfg if it doesn't exist */
    autorun_create_default(NULL);

    serial_puts("[SpiritFoxOS] procfs_init...\n");
    procfs_init();
    printf("[PROCFS] /proc filesystem initialized\n");
}

void init_input(void)
{
    /* 初始化输入核心层与 evdev 字符设备接口。
     * 必须在 init_filesystem() 之后调用：evdev_register_device()
     * 会通过 chardev_create_node() 在 devfs 中创建 /dev/input/eventN
     * 节点，依赖 devfs 已初始化。 */
    serial_puts("[SpiritFoxOS] chardev_init...\n");
    chardev_init();

    serial_puts("[SpiritFoxOS] input_init...\n");
    input_init();

    serial_puts("[SpiritFoxOS] keyboard_input_init...\n");
    keyboard_input_init();

    serial_puts("[SpiritFoxOS] mouse_input_init...\n");
    mouse_input_init();
}

void init_drm(void)
{
    /* 初始化 DRM/KMS 子系统。
     * 必须在 init_filesystem() 之后调用：drm_init() 会通过
     * chardev_create_node() 在 devfs 中创建 /dev/dri/card0、
     * controlD64、renderD128 节点，依赖 devfs 已初始化。 */
    serial_puts("[SpiritFoxOS] drm_init...\n");
    drm_init();
}
