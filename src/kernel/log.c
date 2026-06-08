#include "log.h"
#include "vga.h"
#include "serial.h"
#include "sfs.h"
#include "pmm.h"
#include "vmm.h"
#include "../include/string.h"
#include <stdarg.h>

static log_level_t current_level = LOG_DEBUG;
static char log_buffer[LOG_BUF_SIZE];
static int log_buf_pos = 0;
static int log_line_count = 0;
static int auto_save = 0;
static int save_counter = 0;
#define LOG_AUTO_SAVE_INTERVAL 50

static const char *level_names[] = {
    "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"
};

static const char *level_tags[] = {
    "DBG", "INF", "WRN", "ERR", "FTL"
};

static const vga_color_t level_colors[] = {
    VGA_DARK_GREY,    /* DEBUG: grey */
    VGA_LIGHT_GREY,   /* INFO:  light grey */
    VGA_YELLOW,       /* WARN:  yellow */
    VGA_LIGHT_RED,    /* ERROR: light red */
    VGA_RED            /* FATAL: red on white bg */
};

const char *log_level_name(log_level_t level) {
    if (level >= 0 && level <= LOG_FATAL) return level_names[level];
    return "?????";
}

const char *log_level_color(log_level_t level) {
    if (level >= 0 && level <= LOG_FATAL) return level_tags[level];
    return "???";
}

void log_init(void) {
    log_buf_pos = 0;
    log_line_count = 0;
    memset(log_buffer, 0, LOG_BUF_SIZE);
}

void log_set_level(log_level_t level) {
    current_level = level;
}

log_level_t log_get_level(void) {
    return current_level;
}

static void log_buf_append(char c) {
    if (log_buf_pos < LOG_BUF_SIZE - 1) {
        log_buffer[log_buf_pos++] = c;
    } else {
        int half = LOG_BUF_SIZE / 2;
        memmove(log_buffer, log_buffer + half, half);
        log_buf_pos = half;
        log_buffer[log_buf_pos++] = c;
    }
    if (c == '\n') log_line_count++;
}

static void emit_serial(char c) { serial_putchar(COM1, c); }
static void emit_vga(char c) { vga_putchar(c); }
static void emit_buf(char c) { log_buf_append(c); }

typedef struct {
    void (*fn)(char);
} emit_fn_t;

static void multi_emit(char c, emit_fn_t *emitters, int count) {
    for (int i = 0; i < count; i++) emitters[i].fn(c);
}

void log_write(log_level_t level, const char *module, const char *fmt, ...) {
    if (level < current_level) return;

    extern volatile uint64_t timer_ticks;
    uint64_t secs = timer_ticks / 100;
    uint64_t ms = (timer_ticks % 100) * 10;
    uint64_t mins = secs / 60;
    secs = secs % 60;
    uint64_t hrs = mins / 60;
    mins = mins % 60;

    emit_fn_t emitters[3];
    int emit_count = 0;

    emitters[emit_count++].fn = emit_buf;
    emitters[emit_count++].fn = emit_serial;

    if (level >= LOG_WARN) {
        emitters[emit_count++].fn = emit_vga;
    }

    /* Timestamp */
    char prefix[64];
    int plen = 0;
    {
        char tmp[8];
        if (hrs > 0) {
            if (hrs < 10) prefix[plen++] = '0';
            { int v = (int)hrs; int n = 0; if (v == 0) tmp[n++] = '0'; else while (v > 0) { tmp[n++] = '0' + v % 10; v /= 10; } while (--n >= 0) prefix[plen++] = tmp[n]; }
            prefix[plen++] = ':';
        }
        if (mins < 10) prefix[plen++] = '0';
        { int v = (int)mins; int n = 0; if (v == 0) tmp[n++] = '0'; else while (v > 0) { tmp[n++] = '0' + v % 10; v /= 10; } while (--n >= 0) prefix[plen++] = tmp[n]; }
        prefix[plen++] = ':';
        if (secs < 10) prefix[plen++] = '0';
        { int v = (int)secs; int n = 0; if (v == 0) tmp[n++] = '0'; else while (v > 0) { tmp[n++] = '0' + v % 10; v /= 10; } while (--n >= 0) prefix[plen++] = tmp[n]; }
        prefix[plen++] = '.';
        if (ms < 100) prefix[plen++] = '0';
        if (ms < 10) prefix[plen++] = '0';
        { int v = (int)ms; int n = 0; if (v == 0) tmp[n++] = '0'; else while (v > 0) { tmp[n++] = '0' + v % 10; v /= 10; } while (--n >= 0) prefix[plen++] = tmp[n]; }
        prefix[plen] = '\0';
    }

    for (int i = 0; i < plen; i++) multi_emit(prefix[i], emitters, emit_count);
    multi_emit(' ', emitters, emit_count);

    /* Level tag */
    const char *tag = level_tags[level];
    if (level >= LOG_WARN && emit_count > 2) {
        vga_set_color(level_colors[level], VGA_BLACK);
    }
    multi_emit('[', emitters, emit_count);
    for (int i = 0; tag[i]; i++) multi_emit(tag[i], emitters, emit_count);
    multi_emit(']', emitters, emit_count);
    multi_emit(' ', emitters, emit_count);
    if (level >= LOG_WARN && emit_count > 2) {
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }

    /* Module name */
    if (module) {
        multi_emit('[', emitters, emit_count);
        for (int i = 0; module[i]; i++) multi_emit(module[i], emitters, emit_count);
        multi_emit(']', emitters, emit_count);
        multi_emit(' ', emitters, emit_count);
    }

    /* Message */
    va_list args;
    va_start(args, fmt);

    /* We need a format function that uses multi_emit */
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 'd': case 'i': {
                    int64_t v = va_arg(args, int64_t);
                    if (v < 0) { multi_emit('-', emitters, emit_count); v = -v; }
                    char b[20]; int n = 0;
                    if (v == 0) { multi_emit('0', emitters, emit_count); break; }
                    while (v > 0) { b[n++] = '0' + (v % 10); v /= 10; }
                    while (--n >= 0) multi_emit(b[n], emitters, emit_count);
                    break;
                }
                case 'u': {
                    uint64_t v = va_arg(args, uint64_t);
                    char b[20]; int n = 0;
                    if (v == 0) { multi_emit('0', emitters, emit_count); break; }
                    while (v > 0) { b[n++] = '0' + (v % 10); v /= 10; }
                    while (--n >= 0) multi_emit(b[n], emitters, emit_count);
                    break;
                }
                case 'x': {
                    uint64_t v = va_arg(args, uint64_t);
                    const char h[] = "0123456789ABCDEF";
                    char b[17]; b[16] = '\0';
                    for (int i = 15; i >= 0; i--) { b[i] = h[v & 0xF]; v >>= 4; }
                    int s = 15; while (s > 0 && b[s] == '0') s--;
                    for (int i = s; i < 16; i++) multi_emit(b[i], emitters, emit_count);
                    break;
                }
                case 'p': {
                    uint64_t v = va_arg(args, uint64_t);
                    multi_emit('0', emitters, emit_count);
                    multi_emit('x', emitters, emit_count);
                    const char h[] = "0123456789ABCDEF";
                    char b[17]; b[16] = '\0';
                    for (int i = 15; i >= 0; i--) { b[i] = h[v & 0xF]; v >>= 4; }
                    for (int i = 0; i < 16; i++) multi_emit(b[i], emitters, emit_count);
                    break;
                }
                case 'c': multi_emit((char)va_arg(args, int), emitters, emit_count); break;
                case 's': {
                    const char *s = va_arg(args, const char *);
                    if (s) while (*s) multi_emit(*s++, emitters, emit_count);
                    else { const char *ns = "(null)"; while (*ns) multi_emit(*ns++, emitters, emit_count); }
                    break;
                }
                case '%': multi_emit('%', emitters, emit_count); break;
                default: multi_emit('%', emitters, emit_count); multi_emit(*fmt, emitters, emit_count); break;
            }
        } else {
            multi_emit(*fmt, emitters, emit_count);
        }
        fmt++;
    }
    va_end(args);

    multi_emit('\n', emitters, emit_count);

    /* Auto-save to disk every N log lines */
    if (auto_save) {
        save_counter++;
        if (save_counter >= LOG_AUTO_SAVE_INTERVAL) {
            save_counter = 0;
            log_save_to_disk();
        }
    }
}

int log_read_line(int line_index, char *buf, int buf_size) {
    int current_line = 0;
    int start = -1;
    int i;

    for (i = 0; i < log_buf_pos; i++) {
        if (current_line == line_index && start == -1) {
            start = i;
        }
        if (log_buffer[i] == '\n') {
            if (current_line == line_index) {
                int len = i - start;
                if (len >= buf_size) len = buf_size - 1;
                memcpy(buf, log_buffer + start, len);
                buf[len] = '\0';
                return len;
            }
            current_line++;
        }
    }

    if (start != -1) {
        int len = log_buf_pos - start;
        if (len >= buf_size) len = buf_size - 1;
        memcpy(buf, log_buffer + start, len);
        buf[len] = '\0';
        return len;
    }

    return -1;
}

int log_get_line_count(void) {
    return log_line_count;
}

void log_clear_buffer(void) {
    log_buf_pos = 0;
    log_line_count = 0;
    log_buffer[0] = '\0';
}

void log_dump_buffer(void) {
    serial_puts(COM1, "=== LOG BUFFER DUMP ===\n");
    for (int i = 0; i < log_buf_pos; i++) {
        serial_putchar(COM1, log_buffer[i]);
    }
    serial_puts(COM1, "=== END DUMP ===\n");
}

int log_save_to_disk(void) {
    if (!sfs_is_formatted()) return -1;

    int result = sfs_write_file("system.log", log_buffer, (uint32_t)log_buf_pos);
    if (result == 0) {
        serial_puts(COM1, "[LOG] Saved to system.log\n");
    }
    return result;
}

int log_load_from_disk(void) {
    if (!sfs_is_formatted()) return -1;

    uint32_t size = 0;
    if (sfs_get_file_size("system.log", &size) != 0) return -1;
    if (size == 0 || size > LOG_BUF_SIZE) return -1;

    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) return -1;
    char *tmp = (char *)phys_to_virt(phys);

    uint32_t read_size = 0;
    if (sfs_read_file("system.log", tmp, size, &read_size) != 0) {
        for (uint32_t p = 0; p < pages; p++) pmm_free_page(phys + p * PAGE_SIZE);
        return -1;
    }

    for (uint32_t i = 0; i < read_size && log_buf_pos < LOG_BUF_SIZE - 1; i++) {
        log_buffer[log_buf_pos++] = tmp[i];
        if (tmp[i] == '\n') log_line_count++;
    }

    for (uint32_t p = 0; p < pages; p++) pmm_free_page(phys + p * PAGE_SIZE);
    serial_puts(COM1, "[LOG] Loaded previous log from disk\n");
    return 0;
}

int log_auto_save_enabled(void) {
    return auto_save;
}

void log_set_auto_save(int enabled) {
    auto_save = enabled ? 1 : 0;
    save_counter = 0;
}
