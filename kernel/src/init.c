/*
 * SpiritFoxOS Kernel Subsystem Initialization
 */

#include "boot.h"
#include "gdt.h"
#include "idt.h"
#include "memory.h"
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
#include "smp.h"
#include "slab.h"
#include "rtl8139.h"
#include "serial.h"
#include "autorun.h"

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

    serial_puts("[SpiritFoxOS] smp_init...\n");
    smp_init();

    serial_puts("[SpiritFoxOS] timer_init...\n");
    timer_init();

    serial_puts("[SpiritFoxOS] pci_init...\n");
    pci_init();

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
    ramdisk_create(RAMDISK_DEFAULT_SECTORS);

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
            if (!dev || !dev->in_use || dev->type != BLKDEV_TYPE_AHCI)
                continue;

            char opts[32];
            char num_buf[12];
            utoa((unsigned int)i, num_buf, 10);
            strcpy(opts, "blkdev=");
            strcat(opts, num_buf);

            printf("[FAT32] Trying blkdev=%d...\n", i);
            int mret = vfs_mount(NULL, "/mnt", "fat32", 0, opts);
            if (mret == 0) {
                printf("[FAT32] Mounted at /mnt (blkdev=%d)\n", i);
                mounted = 1;
            } else {
                printf("[FAT32] blkdev=%d mount failed (err=%d), trying next...\n", i, mret);
            }
        }

        if (!mounted) {
            for (int i = 0; i < 16 && !mounted; i++) {
                blkdev_t *dev = blkdev_get((uint8_t)i);
                if (!dev || !dev->in_use || dev->type == BLKDEV_TYPE_AHCI ||
                    dev->type == BLKDEV_TYPE_RAMDISK)
                    continue;

                char opts[32];
                char num_buf[12];
                utoa((unsigned int)i, num_buf, 10);
                strcpy(opts, "blkdev=");
                strcat(opts, num_buf);

                printf("[FAT32] Trying fallback blkdev=%d...\n", i);
                int mret = vfs_mount(NULL, "/mnt", "fat32", 0, opts);
                if (mret == 0) {
                    printf("[FAT32] Mounted at /mnt (fallback blkdev=%d)\n", i);
                    mounted = 1;
                }
            }
        }

        if (!mounted) {
            printf("[FAT32] No suitable block device found for FAT32 mount\n");
        }
    }
}

void init_services(void)
{
    serial_puts("[SpiritFoxOS] kmalloc_init...\n");
    kmalloc_init();

    serial_puts("[SpiritFoxOS] slab_init...\n");
    slab_init();
    printf("[SLAB] Slab allocator initialized\n");

    serial_puts("[SpiritFoxOS] process_init...\n");
    process_init();

    serial_puts("[SpiritFoxOS] syscall_init...\n");
    syscall_init();

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
    vfs_mkdir("/proc", VFS_S_IRUSR | VFS_S_IXUSR);
    vfs_mkdir("/sys", VFS_S_IRUSR | VFS_S_IXUSR);
    printf("[FHS] Standard Linux directory structure created\n");

    /* Create default autorun.cfg if it doesn't exist */
    autorun_create_default(NULL);

    serial_puts("[SpiritFoxOS] procfs_init...\n");
    procfs_init();
    printf("[PROCFS] /proc filesystem initialized\n");
}
