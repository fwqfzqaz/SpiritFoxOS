/*
 * SpiritFoxOS Kernel Entry Point
 */

#include "boot.h"
#include "vga.h"
#include "shell.h"
#include "hal.h"
#include "serial.h"
#include "init.h"
#include "timer.h"
#include "autorun.h"
#include "net.h"
#include "net_icmp.h"

void kernel_panic(const char* msg)
{
    printf("\n!!! KERNEL PANIC: %s !!!\n", msg);
    printf("System Halted.\n");

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void __attribute__((naked, section(".text.entry"))) _start64(void)
{
    __asm__ volatile (
        "movq %rdi, %r12\n\t"
        "movq $0x800000, %rsp\n\t"
        "xorq %rbp, %rbp\n\t"
        "movq $__bss_start, %rdi\n\t"
        "movq $__bss_end, %rcx\n\t"
        "subq %rdi, %rcx\n\t"
        "shrq $3, %rcx\n\t"
        "xorq %rax, %rax\n\t"
        "rep stosq\n\t"
        "movq %r12, %rdi\n\t"
        "call kernel_main\n\t"
        "cli\n\t"
        "1: hlt\n\t"
        "jmp 1b\n\t"
    );
}

BootInfo *g_boot_info;

void kernel_main(BootInfo* boot_info)
{
    g_boot_info = boot_info;

    init_core(boot_info);
    init_hardware();
    init_storage();
    init_filesystem();
    init_services();
    init_fs_hierarchy();

    serial_puts("[SpiritFoxOS] Enabling interrupts...\n");
    hal_enable_interrupts();

    printf("[SpiritFoxOS] All subsystems initialized.\n");
    printf("[SpiritFoxOS] Linux ABI compat | Registry | Packages | Sandbox\n");
    printf("[SpiritFoxOS] System uptime: %llu ms\n", timer_get_ms());

    /* ICMP ping self-test: ping QEMU gateway to verify Echo Request/Reply.
     * Must run AFTER interrupts are enabled so ARP/ICMP replies can be received. */
    if (net_hw_initialized) {
        printf("[NET] ICMP self-test: pinging 10.0.2.2 ...\n");
        for (int i = 0; i < 3; i++) {
            net_icmp_send_echo(net_gateway_ip,
                               (uint16_t)i, 0x5F05, "SpiritFoxOS", 11);
            uint64_t deadline = timer_get_ms() + 2000;
            while (timer_get_ms() < deadline) { hal_io_wait(); }
        }
        printf("[NET] ICMP self-test complete\n");
    }

#ifdef KERNEL_SELFTEST
    extern void test_registry(void);
    extern void test_vfs(void);
    extern void test_fat32(void);
    test_registry();
    test_vfs();
    test_fat32();
#endif

    serial_puts("[SpiritFoxOS] shell_run...\n");
    shell_init();

    /* Execute autorun commands before interactive shell */
    autorun_execute(NULL);

    shell_run();

    kernel_panic("shell_run() returned unexpectedly");
}
