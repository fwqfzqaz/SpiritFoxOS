/* SpiritFoxOS - 初始化进程实现
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
#include "init.h"
#include "shell.h"
#include "gui.h"
#include "vga.h"
#include "keyboard.h"
#include "log.h"
#include "sfs.h"
#include "devtree.h"
#include "../include/io.h"
#include "../include/string.h"

/* ============================================================
 * Init process - the first user process (P)
 * Mounts filesystem and launches the command line interface
 * ============================================================ */

void init_process(void) {
    LOG_I("init", "Init process started (PID 1)");

    /* Mount root filesystem */
    if (sfs_is_formatted()) {
        LOG_I("init", "Root filesystem mounted");
        log_load_from_disk();
        log_set_auto_save(1);
        LOG_I("init", "Auto log-save enabled");
    } else {
        LOG_I("init", "No filesystem available, logs in RAM only");
    }

    /* Launch command line interface */
    LOG_I("init", "Launching command line interface");

    /* Q1: Initialize terminal driver */
    shell_terminal_init();
    LOG_I("init", "[Q1] Terminal driver initialized");

    /* Q2: Start shell interpreter */
    /* Q3: Display command prompt - shell_run handles this */

    /* Enter shell (text mode) - for GUI mode, gui_run is called from kernel_main */
    vga_puts("\n  SpiritFoxOS v1.0.0 - x86_64\n\n");
    vga_puts("Type 'help' for available commands.\n");
    shell_run();
}

/* ============================================================
 * Safe mode - minimal environment when critical devices fail
 * Provides limited shell with only essential functionality
 * ============================================================ */

static void safe_print_banner(void) {
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("\n  ============================================\n");
    vga_puts("  !     SpiritFoxOS - SAFE MODE           !\n");
    vga_puts("  ============================================\n");
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    vga_puts("  One or more critical devices failed self-check.\n");
    vga_puts("  System running in minimal mode.\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_puts("  Available commands: help, reboot, halt, sysinfo\n");
    vga_puts("  ============================================\n\n");
}

void safe_mode_run(void) {
    /* Output device error information to console (L) */
    devtree_print_errors();

    /* Enter minimal safe mode (M) */
    safe_print_banner();

    /* Minimal command loop */
    char cmd_buf[128];
    int cmd_pos = 0;

    while (1) {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("safe# ");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

        cmd_pos = 0;
        while (1) {
            uint16_t key = keyboard_getkey();
            if (IS_SPECIAL_KEY(key)) continue;

            char c = (char)key;
            if (c == '\n') {
                vga_puts("\n");
                cmd_buf[cmd_pos] = '\0';
                break;
            } else if (c == '\b') {
                if (cmd_pos > 0) {
                    cmd_pos--;
                    vga_putchar('\b');
                }
            } else if (c >= ' ' && cmd_pos < 127) {
                cmd_buf[cmd_pos++] = c;
                vga_putchar(c);
            }
        }

        /* Parse and execute minimal commands */
        if (cmd_pos == 0) continue;

        if (strcmp(cmd_buf, "help") == 0) {
            vga_puts("  Safe mode commands:\n");
            vga_puts("    help    - Show this help\n");
            vga_puts("    reboot  - Reboot system\n");
            vga_puts("    halt    - Halt system\n");
            vga_puts("    sysinfo - Show device status\n");
        } else if (strcmp(cmd_buf, "reboot") == 0) {
            vga_puts("Rebooting...\n");
            uint8_t good = 0x02;
            while (good & 0x02)
                good = inb(0x64);
            outb(0x64, 0xFE);
            hlt();
        } else if (strcmp(cmd_buf, "halt") == 0) {
            vga_puts("System halted.\n");
            cli();
            hlt();
        } else if (strcmp(cmd_buf, "sysinfo") == 0) {
            devtree_print_all();
        } else {
            vga_puts("  Unknown command. Type 'help' for available commands.\n");
        }
    }
}
