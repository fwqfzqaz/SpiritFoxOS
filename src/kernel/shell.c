#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "pmm.h"
#include "vmm.h"
#include "scheduler.h"
#include "serial.h"
#include "pic.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/stdarg.h"

/* ============================================================
 * Constants
 * ============================================================ */

#define CMD_BUF_SIZE    256
#define HISTORY_SIZE    32
#define MAX_ARGS        16
#define PROMPT_LEN      20

/* ============================================================
 * Shell state
 * ============================================================ */

static char cmd_buf[CMD_BUF_SIZE];
static int  cmd_pos = 0;

/* Command history (ring buffer) */
static char history[HISTORY_SIZE][CMD_BUF_SIZE];
static int  history_count = 0;   /* Total commands ever entered */
static int  history_view  = 0;   /* Current view index for up/down */

/* Current prompt color theme */
static vga_color_t prompt_user_color  = VGA_LIGHT_GREEN;
static vga_color_t prompt_host_color  = VGA_LIGHT_CYAN;
static vga_color_t prompt_path_color  = VGA_LIGHT_BLUE;
static vga_color_t prompt_sym_color   = VGA_WHITE;

/* ============================================================
 * Q1: Terminal initialization
 * ============================================================ */

void shell_terminal_init(void) {
    /* Reset shell state */
    cmd_pos = 0;
    cmd_buf[0] = '\0';
    history_count = 0;
    history_view = 0;
}

/* ============================================================
 * Output helpers
 * ============================================================ */

static void kputs(const char *s) {
    vga_puts(s);
    serial_puts(COM1, s);
}

static void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vga_printf(fmt, args);
    va_end(args);
}

static void kputchar(char c) {
    vga_putchar(c);
    serial_putchar(COM1, c);
}

/* Print a horizontal rule */
static void print_rule(char ch, int len) {
    for (int i = 0; i < len; i++) kputchar(ch);
    kputchar('\n');
}

/* ============================================================
 * Prompt
 * ============================================================ */

static void print_prompt(void) {
    vga_set_color(prompt_user_color, VGA_BLACK);
    kputs("root");
    vga_set_color(prompt_sym_color, VGA_BLACK);
    kputs("@");
    vga_set_color(prompt_host_color, VGA_BLACK);
    kputs("SpiritFoxOS");
    vga_set_color(prompt_sym_color, VGA_BLACK);
    kputs(":");
    vga_set_color(prompt_path_color, VGA_BLACK);
    kputs("~");
    vga_set_color(prompt_sym_color, VGA_BLACK);
    kputs("# ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* ============================================================
 * Command argument parser
 * ============================================================ */

static char cmd_argv[MAX_ARGS][CMD_BUF_SIZE];
static int  cmd_argc = 0;

static void parse_args(const char *buf) {
    cmd_argc = 0;
    int arg_pos = 0;

    while (*buf && cmd_argc < MAX_ARGS) {
        /* Skip whitespace */
        while (*buf == ' ' || *buf == '\t') buf++;
        if (!*buf) break;

        arg_pos = 0;
        /* Handle quoted strings */
        if (*buf == '"') {
            buf++; /* skip opening quote */
            while (*buf && *buf != '"' && arg_pos < CMD_BUF_SIZE - 1) {
                cmd_argv[cmd_argc][arg_pos++] = *buf++;
            }
            if (*buf == '"') buf++; /* skip closing quote */
        } else {
            while (*buf && *buf != ' ' && *buf != '\t' && arg_pos < CMD_BUF_SIZE - 1) {
                cmd_argv[cmd_argc][arg_pos++] = *buf++;
            }
        }
        cmd_argv[cmd_argc][arg_pos] = '\0';
        cmd_argc++;
    }
}

/* ============================================================
 * Tab completion
 * ============================================================ */

static const char * const builtin_cmds[] = {
    "help", "clear", "uname", "uptime", "mem", "ps", "reboot",
    "halt", "shutdown", "echo", "whoami", "hostname", "date",
    "cpuinfo", "history", "color", "neofetch", "about", "sysinfo",
    "vmstat", "free", "kill", "sleep", "beep", "cowsay", NULL
};

static int count_matching(const char *prefix, int prefix_len) {
    int count = 0;
    for (int i = 0; builtin_cmds[i]; i++) {
        if (strncmp(builtin_cmds[i], prefix, (size_t)prefix_len) == 0)
            count++;
    }
    return count;
}

static const char *find_unique_match(const char *prefix, int prefix_len) {
    const char *match = NULL;
    for (int i = 0; builtin_cmds[i]; i++) {
        if (strncmp(builtin_cmds[i], prefix, (size_t)prefix_len) == 0) {
            if (match) return NULL; /* Ambiguous */
            match = builtin_cmds[i];
        }
    }
    return match;
}

static void handle_tab_complete(void) {
    if (cmd_pos == 0) return;

    /* Find the start of the current word */
    int word_start = cmd_pos;
    while (word_start > 0 && cmd_buf[word_start - 1] != ' ')
        word_start--;
    int word_len = cmd_pos - word_start;

    int matches = count_matching(&cmd_buf[word_start], word_len);
    if (matches == 0) return;

    if (matches == 1) {
        /* Unique match - complete it */
        const char *match = find_unique_match(&cmd_buf[word_start], word_len);
        if (match) {
            for (int i = word_len; match[i]; i++) {
                cmd_buf[cmd_pos] = match[i];
                kputchar(match[i]);
                cmd_pos++;
            }
            cmd_buf[cmd_pos] = ' ';
            kputchar(' ');
            cmd_pos++;
            cmd_buf[cmd_pos] = '\0';
        }
    } else {
        /* Multiple matches - show them */
        kputs("\n");
        for (int i = 0; builtin_cmds[i]; i++) {
            if (strncmp(builtin_cmds[i], &cmd_buf[word_start], (size_t)word_len) == 0) {
                kputs(builtin_cmds[i]);
                kputs("  ");
            }
        }
        kputs("\n");
        print_prompt();
        kputs(cmd_buf);
    }
}

/* ============================================================
 * Command history
 * ============================================================ */

static void history_push(const char *cmd) {
    if (cmd[0] == '\0') return;
    int idx = history_count % HISTORY_SIZE;
    strcpy(history[idx], cmd);
    history_count++;
    history_view = history_count;
}

static const char *history_get(int offset) {
    if (offset <= 0 || offset > history_count) return "";
    int idx = (offset - 1) % HISTORY_SIZE;
    return history[idx];
}

static void clear_current_line(void) {
    /* Move cursor back to after prompt and clear */
    while (cmd_pos > 0) {
        kputchar('\b');
        cmd_pos--;
    }
    /* Overwrite with spaces */
    for (int i = 0; i < CMD_BUF_SIZE - 1; i++) {
        kputchar(' ');
    }
    /* Move back again */
    for (int i = 0; i < CMD_BUF_SIZE - 1; i++) {
        kputchar('\b');
    }
    cmd_pos = 0;
    cmd_buf[0] = '\0';
}

/* ============================================================
 * Built-in commands
 * ============================================================ */

static void cmd_help(void);
static void cmd_uname(void);
static void cmd_uptime(void);
static void cmd_mem(void);
static void cmd_free(void);
static void cmd_ps(void);
static void cmd_echo(void);
static void cmd_whoami(void);
static void cmd_hostname(void);
static void cmd_date(void);
static void cmd_cpuinfo(void);
static void cmd_history(void);
static void cmd_color(void);
static void cmd_neofetch(void);
static void cmd_about(void);
static void cmd_sysinfo(void);
static void cmd_vmstat(void);
static void cmd_reboot(void);
static void cmd_halt(void);
static void cmd_shutdown(void);
static void cmd_beep(void);
static void cmd_cowsay(void);
static void cmd_clear(void);
static void cmd_kill(void);
static void cmd_sleep(void);

/* ============================================================
 * Command implementations
 * ============================================================ */

static void cmd_help(void) {
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    kputs("\n  SpiritFoxOS Shell - Command Reference\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    print_rule('-', 42);

    vga_set_color(VGA_WHITE, VGA_BLACK);
    kputs("  GENERAL\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kputs("    help          Show this help message\n");
    kputs("    clear         Clear the terminal screen\n");
    kputs("    echo <text>   Display a line of text\n");
    kputs("    history       Show command history\n");
    kputs("    color <fg>    Change text color (0-15)\n");
    kputs("    about         About SpiritFoxOS\n");
    kputs("    neofetch      System information display\n");
    kputs("    cowsay <msg>  ASCII cow says your message\n");
    kputs("    beep          Produce a beep sound\n");

    vga_set_color(VGA_WHITE, VGA_BLACK);
    kputs("\n  SYSTEM INFORMATION\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kputs("    uname         Print system information\n");
    kputs("    sysinfo       Detailed system overview\n");
    kputs("    cpuinfo       Display CPU information\n");
    kputs("    date          Show current date/time\n");
    kputs("    uptime        Show system uptime\n");
    kputs("    whoami        Print current user\n");
    kputs("    hostname      Print system hostname\n");

    vga_set_color(VGA_WHITE, VGA_BLACK);
    kputs("\n  MEMORY & PROCESSES\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kputs("    mem           Show memory statistics\n");
    kputs("    free          Show free memory (detailed)\n");
    kputs("    vmstat        Virtual memory statistics\n");
    kputs("    ps            List running processes\n");
    kputs("    kill <pid>    Terminate a process\n");
    kputs("    sleep <sec>   Sleep for N seconds\n");

    vga_set_color(VGA_WHITE, VGA_BLACK);
    kputs("\n  POWER MANAGEMENT\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kputs("    reboot        Restart the system\n");
    kputs("    halt          Halt the CPU\n");
    kputs("    shutdown      Power off the system\n");
    kputchar('\n');
}

static void cmd_uname(void) {
    if (cmd_argc > 1 && strcmp(cmd_argv[1], "-a") == 0) {
        kputs("SpiritFoxOS 1.0.0 SpiritFoxOS-kernel x86_64 GNU/Linux-compatible\n");
    } else {
        kputs("SpiritFoxOS\n");
    }
}

static void cmd_uptime(void) {
    extern volatile uint64_t timer_ticks;
    uint64_t ticks = timer_ticks;
    uint32_t secs = (uint32_t)(ticks / 100);
    uint32_t mins = secs / 60;
    uint32_t hours = mins / 60;
    uint32_t days = hours / 24;

    kputs(" ");
    if (days > 0) {
        vga_printf("%u day%s, ", (uint32_t)days, days > 1 ? "s" : "");
    }
    vga_printf("%02u:%02u:%02u", (uint32_t)(hours % 24), (uint32_t)(mins % 60), (uint32_t)(secs % 60));
    kprintf(" up %u seconds\n", secs);
}

static void cmd_mem(void) {
    uint64_t free_pages = pmm_free_count();
    uint64_t total_pages = pmm_total_count();
    uint64_t used_pages = total_pages - free_pages;
    uint64_t total_kb = total_pages * 4;
    uint64_t free_kb = free_pages * 4;
    uint64_t used_kb = used_pages * 4;

    vga_set_color(VGA_WHITE, VGA_BLACK);
    kputs("              total       used       free\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("Mem:    %8u KB %8u KB %8u KB\n",
            (uint32_t)total_kb, (uint32_t)used_kb, (uint32_t)free_kb);

    /* Usage bar */
    uint32_t pct = total_pages > 0 ? (uint32_t)((used_pages * 100) / total_pages) : 0;
    kputs("Usage: [");
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    int filled = pct / 5;
    for (int i = 0; i < 20; i++) {
        kputchar(i < filled ? '#' : '-');
    }
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("] %u%%\n", pct);
}

static void cmd_free(void) {
    cmd_mem(); /* Alias with detailed view */
}

static void cmd_ps(void) {
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kputs("  PID  STATE     TIME\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    print_rule('-', 30);

    extern volatile uint64_t timer_ticks;
    process_t *cur = scheduler_current();
    if (cur) {
        vga_printf("  %3u  RUNNING   %us\n",
                   (uint32_t)cur->pid,
                   (uint32_t)(timer_ticks / 100));
    } else {
        kputs("  (no process running)\n");
    }
}

static void cmd_echo(void) {
    for (int i = 1; i < cmd_argc; i++) {
        if (i > 1) kputchar(' ');
        kputs(cmd_argv[i]);
    }
    kputchar('\n');
}

static void cmd_whoami(void) {
    kputs("root\n");
}

static void cmd_hostname(void) {
    if (cmd_argc > 1) {
        kputs("hostname: cannot set hostname (read-only)\n");
    } else {
        kputs("SpiritFoxOS\n");
    }
}

static void cmd_date(void) {
    extern volatile uint64_t timer_ticks;
    uint32_t secs = (uint32_t)(timer_ticks / 100);
    uint32_t mins = secs / 60;
    uint32_t hours = mins / 60;

    kprintf("System timer: %u ticks (%u:%02u:%02u since boot)\n",
            (uint32_t)timer_ticks,
            (uint32_t)(hours % 24), (uint32_t)(mins % 60), (uint32_t)(secs % 60));
    kputs("(No RTC driver - time relative to boot)\n");
}

static void cmd_cpuinfo(void) {
    uint32_t eax, ebx, ecx, edx;

    vga_set_color(VGA_WHITE, VGA_BLACK);
    kputs("\n  CPU Information\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    print_rule('-', 35);

    /* Get CPU vendor string */
    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0)
    );
    char vendor[13];
    memcpy(vendor, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';

    kputs("  Vendor:  ");
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs(vendor);
    kputchar('\n');
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    /* Get CPU model */
    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x80000000)
    );
    char model[49];
    if (eax >= 0x80000004) {
        uint32_t regs[12];
        for (int i = 0; i < 3; i++) {
            __asm__ volatile (
                "cpuid"
                : "=a"(regs[i*4]), "=b"(regs[i*4+1]), "=c"(regs[i*4+2]), "=d"(regs[i*4+3])
                : "a"((uint32_t)(0x80000002 + i))
            );
        }
        memcpy(model, regs, 48);
        model[48] = '\0';
        /* Trim leading spaces */
        char *p = model;
        while (*p == ' ') p++;
        kputs("  Model:   ");
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        kputs(p);
        kputchar('\n');
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }

    kputs("  Arch:    x86_64 (Long Mode)\n");
    kputs("  Mode:    64-bit kernel\n");
    kputchar('\n');
}

static void cmd_history(void) {
    int start = 1;
    if (history_count > HISTORY_SIZE) {
        start = history_count - HISTORY_SIZE + 1;
    }
    for (int i = start; i <= history_count; i++) {
        vga_printf("  %4d  %s\n", (uint32_t)i, history_get(i));
    }
}

static void cmd_color(void) {
    if (cmd_argc < 2) {
        kputs("Usage: color <fg> [bg]\n");
        kputs("Colors: 0=Black 1=Blue 2=Green 3=Cyan 4=Red 5=Magenta\n");
        kputs("        6=Brown 7=LightGrey 8=DarkGrey 9=LightBlue\n");
        kputs("        10=LightGreen 11=LightCyan 12=LightRed\n");
        kputs("        13=LightMagenta 14=Yellow 15=White\n");
        return;
    }
    /* Parse foreground color number */
    int fg = 0;
    const char *s = cmd_argv[1];
    while (*s >= '0' && *s <= '9') {
        fg = fg * 10 + (*s - '0');
        s++;
    }
    if (fg < 0 || fg > 15) {
        kputs("Invalid color value (0-15)\n");
        return;
    }
    int bg = VGA_BLACK;
    if (cmd_argc >= 3) {
        bg = 0;
        s = cmd_argv[2];
        while (*s >= '0' && *s <= '9') {
            bg = bg * 10 + (*s - '0');
            s++;
        }
        if (bg < 0 || bg > 15) bg = VGA_BLACK;
    }
    vga_set_color((vga_color_t)fg, (vga_color_t)bg);
    kputs("Color changed.\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_neofetch(void) {
    extern volatile uint64_t timer_ticks;
    extern char _start[];
    extern char _end[];
    uint64_t free_pages = pmm_free_count();
    uint64_t total_pages = pmm_total_count();
    uint32_t secs = (uint32_t)(timer_ticks / 100);

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs("\n");
    kputs("        ,--.        ,--.          "); vga_set_color(VGA_WHITE, VGA_BLACK); kputs("root@SpiritFoxOS\n");
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs("        |  |        |  |          "); vga_set_color(VGA_WHITE, VGA_BLACK); kputs("--------------\n");
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs("     ,--|  |--,  ,--|  |--,       "); vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK); kputs("OS: "); vga_set_color(VGA_LIGHT_GREY, VGA_BLACK); kputs("SpiritFoxOS 1.0.0 x86_64\n");
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs("    /   |  |  \\/  |  |   \\      "); vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK); kputs("Kernel: "); vga_set_color(VGA_LIGHT_GREY, VGA_BLACK); kputs("SpiritFoxOS-kernel 1.0.0\n");
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs("   /    |  |      |  |    \\     "); vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK); kputs("Uptime: "); vga_set_color(VGA_LIGHT_GREY, VGA_BLACK); vga_printf("%u secs\n", secs);
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs("  |     |  |      |  |     |    "); vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK); kputs("Shell: "); vga_set_color(VGA_LIGHT_GREY, VGA_BLACK); kputs("sfsh 1.0\n");
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs("  |     |  |      |  |     |    "); vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK); kputs("Resolution: "); vga_set_color(VGA_LIGHT_GREY, VGA_BLACK); kputs("80x25\n");
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs("   \\    |  |      |  |    /     "); vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK); kputs("Memory: "); vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_printf("%u MB / %u MB\n",
               (uint32_t)((total_pages - free_pages) * 4 / 1024),
               (uint32_t)(total_pages * 4 / 1024));
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs("    \\   |__|      |__|   /      "); vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK); kputs("Kernel size: "); vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_printf("%u KB\n", (uint32_t)((_end - _start) / 1024));
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs("     `--|__|      |__|--'       \n");
    kputs("        |  |      |  |          ");
    /* Color palette */
    for (int i = 0; i < 8; i++) {
        vga_set_color((vga_color_t)i, (vga_color_t)i);
        kputs("  ");
    }
    kputchar('\n');
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs("        `--'      `--'          ");
    for (int i = 8; i < 16; i++) {
        vga_set_color((vga_color_t)i, (vga_color_t)i);
        kputs("  ");
    }
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kputs("\n\n");
}

static void cmd_about(void) {
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    kputs("\n  SpiritFoxOS v1.0.0\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kputs("  A hobby x86_64 operating system written from scratch.\n");
    kputs("  Features: Multiboot2 boot, GDT/IDT, VGA text mode,\n");
    kputs("  PMM/VMM, round-robin scheduler, PS/2 keyboard, serial I/O.\n\n");
}

static void cmd_sysinfo(void) {
    extern volatile uint64_t timer_ticks;
    extern char _start[];
    extern char _end[];
    uint64_t free_pages = pmm_free_count();
    uint64_t total_pages = pmm_total_count();

    vga_set_color(VGA_WHITE, VGA_BLACK);
    kputs("\n  System Information\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    print_rule('=', 40);
    vga_printf("  OS:          SpiritFoxOS v1.0.0\n");
    vga_printf("  Architecture:x86_64\n");
    vga_printf("  Kernel:      0x%x - 0x%x (%u KB)\n",
               (uint32_t)(uint64_t)_start, (uint32_t)(uint64_t)_end,
               (uint32_t)((_end - _start) / 1024));
    vga_printf("  Uptime:      %u seconds\n", (uint32_t)(timer_ticks / 100));
    vga_printf("  Memory:      %u MB total, %u MB free\n",
               (uint32_t)(total_pages * 4 / 1024),
               (uint32_t)(free_pages * 4 / 1024));
    vga_printf("  Timer:       100 Hz (PIT)\n");
    vga_printf("  Display:     VGA text 80x25\n");
    vga_printf("  Shell:       sfsh 1.0\n");
    kputchar('\n');
}

static void cmd_vmstat(void) {
    uint64_t free_pages = pmm_free_count();
    uint64_t total_pages = pmm_total_count();
    uint64_t used_pages = total_pages - free_pages;

    vga_set_color(VGA_WHITE, VGA_BLACK);
    kputs("  Virtual Memory Statistics\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    print_rule('-', 35);
    vga_printf("  Total pages:   %u\n", (uint32_t)total_pages);
    vga_printf("  Used pages:    %u\n", (uint32_t)used_pages);
    vga_printf("  Free pages:    %u\n", (uint32_t)free_pages);
    vga_printf("  Page size:     4096 bytes\n");
    vga_printf("  Paging mode:   4-level (PML4)\n");
    vga_printf("  Address bits:  48 (canonical)\n");
    kputchar('\n');
}

static void cmd_reboot(void) {
    kputs("Rebooting system...\n");
    uint8_t good = 0x02;
    while (good & 0x02)
        good = inb(0x64);
    outb(0x64, 0xFE);
    hlt();
}

static void cmd_halt(void) {
    kputs("System halted.\n");
    cli();
    hlt();
}

static void cmd_shutdown(void) {
    kputs("Shutting down...\n");
    /* Try ACPI shutdown (QEMU supports this via port 0x604) */
    outw(0x604, 0x2000);
    cli();
    hlt();
}

static void cmd_beep(void) {
    /* Play a beep through the PC speaker */
    uint8_t val = inb(0x61);
    outb(0x61, val | 0x03);  /* Enable speaker */
    /* Set frequency via PIT channel 2 */
    outb(0x43, 0xB6);
    outb(0x42, 0x90);  /* Divisor low byte (approx 1000Hz) */
    outb(0x42, 0x03);  /* Divisor high byte */
    /* Short delay */
    for (volatile int i = 0; i < 5000000; i++);
    outb(0x61, val);  /* Disable speaker */
    kputs("Beep!\n");
}

static void cmd_cowsay(void) {
    if (cmd_argc < 2) {
        kputs("Usage: cowsay <message>\n");
        return;
    }

    /* Calculate total message length */
    int msg_len = 0;
    for (int i = 1; i < cmd_argc; i++) {
        msg_len += (int)strlen(cmd_argv[i]);
        if (i < cmd_argc - 1) msg_len++; /* space */
    }
    if (msg_len > 60) msg_len = 60;

    /* Top border */
    kputchar(' ');
    for (int i = 0; i < msg_len + 2; i++) kputchar('_');
    kputchar('\n');

    /* Message */
    kputs("< ");
    for (int i = 1; i < cmd_argc; i++) {
        if (i > 1) kputchar(' ');
        kputs(cmd_argv[i]);
    }
    kputs(" >\n");

    /* Bottom border */
    kputchar(' ');
    for (int i = 0; i < msg_len + 2; i++) kputchar('-');
    kputchar('\n');

    /* Cow */
    kputs("        \\   ^__^\n");
    kputs("         \\  (oo)\\_______\n");
    kputs("            (__)\\       )\\/\\\n");
    kputs("                ||----w |\n");
    kputs("                ||     ||\n");
}

static void cmd_clear(void) {
    vga_clear();
}

static void cmd_kill(void) {
    if (cmd_argc < 2) {
        kputs("Usage: kill <pid>\n");
        return;
    }
    /* Parse PID */
    int pid = 0;
    const char *s = cmd_argv[1];
    while (*s >= '0' && *s <= '9') {
        pid = pid * 10 + (*s - '0');
        s++;
    }
    if (pid <= 0) {
        kputs("Invalid PID\n");
        return;
    }
    process_t *cur = scheduler_current();
    if (cur && (uint64_t)pid == cur->pid) {
        kputs("Cannot kill the current process (kernel shell)\n");
    } else {
        kprintf("kill: PID %u not found or not supported\n", (uint32_t)pid);
    }
}

static void cmd_sleep(void) {
    if (cmd_argc < 2) {
        kputs("Usage: sleep <seconds>\n");
        return;
    }
    int secs = 0;
    const char *s = cmd_argv[1];
    while (*s >= '0' && *s <= '9') {
        secs = secs * 10 + (*s - '0');
        s++;
    }
    if (secs <= 0 || secs > 30) {
        kputs("sleep: invalid duration (1-30 seconds)\n");
        return;
    }
    extern volatile uint64_t timer_ticks;
    uint64_t target = timer_ticks + (uint64_t)secs * 100;
    while (timer_ticks < target) {
        hlt();
    }
}

/* ============================================================
 * Command dispatcher
 * ============================================================ */

typedef struct {
    const char *name;
    void (*handler)(void);
    const char *short_help;
} command_t;

static const command_t commands[] = {
    {"help",     cmd_help,     "Show help"},
    {"clear",    cmd_clear,    "Clear screen"},
    {"uname",    cmd_uname,    "System info"},
    {"uptime",   cmd_uptime,   "System uptime"},
    {"mem",      cmd_mem,      "Memory info"},
    {"free",     cmd_free,     "Free memory"},
    {"ps",       cmd_ps,       "Process list"},
    {"echo",     cmd_echo,     "Echo text"},
    {"whoami",   cmd_whoami,   "Current user"},
    {"hostname", cmd_hostname, "System hostname"},
    {"date",     cmd_date,     "Date/time"},
    {"cpuinfo",  cmd_cpuinfo,  "CPU info"},
    {"history",  cmd_history,  "Command history"},
    {"color",    cmd_color,    "Change color"},
    {"neofetch", cmd_neofetch, "System info display"},
    {"about",    cmd_about,    "About SpiritFoxOS"},
    {"sysinfo",  cmd_sysinfo,  "System overview"},
    {"vmstat",   cmd_vmstat,   "VM statistics"},
    {"reboot",   cmd_reboot,   "Reboot system"},
    {"halt",     cmd_halt,     "Halt CPU"},
    {"shutdown", cmd_shutdown, "Power off"},
    {"beep",     cmd_beep,     "Beep sound"},
    {"cowsay",   cmd_cowsay,   "ASCII cow"},
    {"kill",     cmd_kill,     "Kill process"},
    {"sleep",    cmd_sleep,    "Sleep N seconds"},
    {NULL, NULL, NULL}
};

static void execute_command(void) {
    if (cmd_pos == 0) return;
    cmd_buf[cmd_pos] = '\0';

    parse_args(cmd_buf);
    if (cmd_argc == 0) return;

    /* Search for command */
    for (int i = 0; commands[i].name; i++) {
        if (strcmp(cmd_argv[0], commands[i].name) == 0) {
            commands[i].handler();
            return;
        }
    }

    /* Unknown command */
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kputs("sfsh: command not found: ");
    kputs(cmd_argv[0]);
    kputchar('\n');
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    /* Suggest similar commands */
    int prefix_len = (int)strlen(cmd_argv[0]);
    if (prefix_len >= 2) {
        int found = 0;
        for (int i = 0; commands[i].name; i++) {
            if (strncmp(commands[i].name, cmd_argv[0], (size_t)prefix_len) == 0) {
                if (!found) {
                    kputs("Did you mean: ");
                    found = 1;
                }
                kputs(commands[i].name);
                kputs("  ");
            }
        }
        if (found) kputchar('\n');
    }
}

/* ============================================================
 * Main shell loop
 * ============================================================ */

void shell_run(void) {
    kputchar('\n');
    print_prompt();

    while (1) {
        uint16_t key = keyboard_getkey();

        if (IS_SPECIAL_KEY(key)) {
            switch (key) {
                case KEY_UP:
                    if (history_view > 1) {
                        history_view--;
                        clear_current_line();
                        strcpy(cmd_buf, history_get(history_view));
                        cmd_pos = (int)strlen(cmd_buf);
                        kputs(cmd_buf);
                    }
                    break;

                case KEY_DOWN:
                    if (history_view < history_count) {
                        history_view++;
                        clear_current_line();
                        strcpy(cmd_buf, history_get(history_view));
                        cmd_pos = (int)strlen(cmd_buf);
                        kputs(cmd_buf);
                    } else if (history_view == history_count) {
                        history_view = history_count + 1;
                        clear_current_line();
                    }
                    break;

                case KEY_LEFT:
                    /* Cursor left - not yet supported in VGA text mode */
                    break;

                case KEY_RIGHT:
                    /* Cursor right - not yet supported */
                    break;

                case KEY_HOME:
                    clear_current_line();
                    if (cmd_pos > 0) {
                        strcpy(cmd_buf, cmd_buf); /* Keep content */
                        kputs(cmd_buf);
                    }
                    break;

                case KEY_TAB:
                    handle_tab_complete();
                    break;

                case KEY_CTRL_C:
                    /* Cancel current line */
                    kputs("^C\n");
                    cmd_pos = 0;
                    cmd_buf[0] = '\0';
                    print_prompt();
                    break;
            }
        } else {
            char c = (char)key;

            if (c == '\n') {
                kputchar('\n');
                cmd_buf[cmd_pos] = '\0';
                history_push(cmd_buf);
                execute_command();
                cmd_pos = 0;
                cmd_buf[0] = '\0';
                print_prompt();
            } else if (c == '\b') {
                if (cmd_pos > 0) {
                    cmd_pos--;
                    cmd_buf[cmd_pos] = '\0';
                    kputchar('\b');
                }
            } else if (c >= ' ' && cmd_pos < CMD_BUF_SIZE - 1) {
                cmd_buf[cmd_pos] = c;
                cmd_pos++;
                cmd_buf[cmd_pos] = '\0';
                kputchar(c);
            }
        }
    }
}
