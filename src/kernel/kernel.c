#include "../include/multiboot2.h"
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
#include "pmm.h"
#include "vmm.h"
#include "scheduler.h"
#include "serial.h"
#include "shell.h"
#include "gui.h"

/* Kernel start/end symbols from linker script */
extern char _start[];
extern char _end[];

/* Timer tick counter */
volatile uint64_t timer_ticks = 0;

/* Flag: are we in graphical mode? */
static int graphical_mode = 0;

/* Dual output: VGA + Serial (only VGA if text mode) */
static void kputs(const char *str) {
    if (!graphical_mode) vga_puts(str);
    serial_puts(COM1, str);
}

static void __attribute__((unused)) kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (!graphical_mode) vga_printf(fmt, args);
    va_end(args);
}

/* Timer interrupt handler */
static void timer_handler(struct interrupt_frame *frame) {
    timer_ticks++;
    scheduler_tick(frame);
    pic_send_eoi(0);
}

/* Main kernel entry point (called from boot.asm) */
void kernel_main(uint32_t multiboot2_magic, uint32_t multiboot2_info_addr) {
    (void)multiboot2_magic;
    /* Initialize serial port first for debug output */
    serial_init(COM1);
    serial_puts(COM1, "SpiritFoxOS: Serial initialized\n");

    /* First, parse Multiboot2 info to detect framebuffer */
    struct multiboot2_tag *tag;
    struct multiboot2_tag_mmap *mmap_tag = NULL;
    struct multiboot2_tag_framebuffer *fb_tag = NULL;

    FOR_EACH_TAG(tag, (void *)(uint64_t)multiboot2_info_addr) {
        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            mmap_tag = (struct multiboot2_tag_mmap *)tag;
        }
        if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER) {
            fb_tag = (struct multiboot2_tag_framebuffer *)tag;
        }
    }

    /* Determine if we have a graphical framebuffer */
    if (fb_tag && fb_tag->framebuffer_type == 1) {
        graphical_mode = 1;
        serial_puts(COM1, "DBG: Graphical framebuffer detected\n");
    }

    /* Initialize VGA only in text mode */
    if (!graphical_mode) {
        vga_init();
        vga_set_color(VGA_WHITE, VGA_BLUE);
        vga_puts(" SpiritFoxOS Kernel v1.0.0 - x86_64 ");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        vga_puts("\n\n");
    }

    serial_puts(COM1, "DBG: Before GDT init\n");

    /* Initialize GDT */
    gdt_init();
    kputs("[OK] GDT initialized\n");

    /* Initialize IDT */
    idt_init();
    kputs("[OK] IDT initialized\n");

    /* Initialize PIC */
    pic_init();
    kputs("[OK] PIC initialized\n");

    /* Initialize PIT at 100Hz */
    pit_init(100);
    idt_register_handler(32, timer_handler);
    pic_unmask_irq(0);
    kputs("[OK] PIT timer at 100Hz\n");

    if (!mmap_tag) {
        kputs("ERROR: No memory map from bootloader!\n");
        hlt();
    }

    /* Initialize Physical Memory Manager */
    uint64_t kernel_start = (uint64_t)_start;
    uint64_t kernel_end = (uint64_t)_end;
    pmm_init(mmap_tag, kernel_start, kernel_end);
    kputs("[OK] PMM initialized\n");

    /* Initialize Virtual Memory Manager */
    vmm_init();
    kputs("[OK] VMM initialized\n");

    /* Initialize Keyboard */
    keyboard_init();
    kputs("[OK] Keyboard driver loaded\n");

    /* Initialize Scheduler */
    scheduler_init();
    kputs("[OK] Process scheduler initialized\n");

    /* Enable interrupts */
    sti();
    kputs("[OK] Interrupts enabled\n");

    /* Check if framebuffer is available for GUI mode */
    if (fb_tag && fb_tag->framebuffer_type == 1) {
        serial_puts(COM1, "DBG: Launching GUI...\n");

        gui_init(fb_tag->framebuffer_addr,
                 fb_tag->framebuffer_width,
                 fb_tag->framebuffer_height,
                 fb_tag->framebuffer_pitch,
                 fb_tag->framebuffer_bpp);
        gui_run();
    } else {
        /* No framebuffer - text mode shell */
        kputs("\n  SpiritFoxOS v1.0.0 - x86_64\n\n");
        kputs("Type 'help' for available commands.\n");
        shell_run();
    }

    while (1) hlt();
}
