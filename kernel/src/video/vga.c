#include "vga.h"
#include "hal.h"
#include "fb.h"
#include <stdarg.h>

/* 串口（COM1）用于QEMU调试输出 */
#define COM1 0x3F8

static inline void serial_putchar(char c)
{
    while (!(hal_inb(COM1 + 5) & 0x20))
        ;
    hal_outb(COM1, (uint8_t)c);
}

static void serial_init(void)
{
    hal_outb(COM1 + 1, 0x00);
    hal_outb(COM1 + 3, 0x80);
    hal_outb(COM1 + 0, 0x03);
    hal_outb(COM1 + 1, 0x00);
    hal_outb(COM1 + 3, 0x03);
    hal_outb(COM1 + 2, 0xC7);
    hal_outb(COM1 + 4, 0x0B);
}

/* 文本模式VGA */
#define VGA_TEXT_ADDR   0xB8000
#define VGA_COLS        80
#define VGA_ROWS        25

/* VGA颜色属性 */
#define VGA_ATTR(fg, bg) ((uint8_t)(((bg) << 4) | ((fg) & 0x0F)))
#define VGA_WHITE_ON_BLACK VGA_ATTR(0x0F, 0x00)

/* 光标控制端口 */
#define VGA_CTRL_REGISTER 0x3D4
#define VGA_DATA_REGISTER 0x3D5
#define VGA_CURSOR_HIGH   14
#define VGA_CURSOR_LOW    15

static volatile uint16_t* const vga_buffer = (volatile uint16_t*)VGA_TEXT_ADDR;

static int cursor_x;
static int cursor_y;
static uint8_t text_attr;

static void update_cursor(void)
{
    uint16_t pos = cursor_y * VGA_COLS + cursor_x;
    hal_outb(VGA_CTRL_REGISTER, VGA_CURSOR_HIGH);
    hal_outb(VGA_DATA_REGISTER, (uint8_t)(pos >> 8));
    hal_outb(VGA_CTRL_REGISTER, VGA_CURSOR_LOW);
    hal_outb(VGA_DATA_REGISTER, (uint8_t)(pos & 0xFF));
}

static void scroll(void)
{
    /* 所有行上移一行 */
    for (int y = 0; y < VGA_ROWS - 1; y++) {
        for (int x = 0; x < VGA_COLS; x++) {
            int src = (y + 1) * VGA_COLS + x;
            int dst = y * VGA_COLS + x;
            vga_buffer[dst] = vga_buffer[src];
        }
    }

    /* 清除最后一行 */
    for (int x = 0; x < VGA_COLS; x++) {
        vga_buffer[(VGA_ROWS - 1) * VGA_COLS + x] = (uint16_t)text_attr << 8 | ' ';
    }

    cursor_y = VGA_ROWS - 1;
}

void vga_init(BootInfo* info)
{
    text_attr = VGA_WHITE_ON_BLACK;
    cursor_x = 0;
    cursor_y = 0;

    vga_clear();
    serial_init();

    /* 初始化帧缓冲驱动。UEFI启动时使用GOP提供的帧缓冲；
     * BIOS启动时使用VBE DISPI。 */
    fb_init(info);

    /* 如果帧缓冲已初始化，则设置帧缓冲文本终端 */
    if (fb_term_is_active() == 0) {
        fb_term_init();
    }
}

void vga_putchar(char c)
{
    /* 始终输出到串口用于QEMU日志 */
    serial_putchar(c);

    /* 如果帧缓冲终端激活，则渲染到帧缓冲而非VGA文本缓冲 */
    if (fb_term_is_active()) {
        fb_term_putchar(c);
        return;
    }

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        cursor_x = (cursor_x + 4) & ~3;
        if (cursor_x >= VGA_COLS) {
            cursor_x = 0;
            cursor_y++;
        }
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            vga_buffer[cursor_y * VGA_COLS + cursor_x] = (uint16_t)text_attr << 8 | ' ';
        }
    } else {
        vga_buffer[cursor_y * VGA_COLS + cursor_x] = (uint16_t)text_attr << 8 | (uint8_t)c;
        cursor_x++;
        if (cursor_x >= VGA_COLS) {
            cursor_x = 0;
            cursor_y++;
        }
    }

    if (cursor_y >= VGA_ROWS) {
        scroll();
    }

    update_cursor();
}

void vga_print(const char* str)
{
    while (*str) {
        vga_putchar(*str);
        str++;
    }
}

void vga_clear(void)
{
    for (int y = 0; y < VGA_ROWS; y++) {
        for (int x = 0; x < VGA_COLS; x++) {
            vga_buffer[y * VGA_COLS + x] = (uint16_t)text_attr << 8 | ' ';
        }
    }
    cursor_x = 0;
    cursor_y = 0;
    update_cursor();
}

void vga_set_color(uint32_t fg, uint32_t bg)
{
    text_attr = VGA_ATTR(fg & 0x0F, bg & 0x0F);
}

void vga_set_cursor(int x, int y)
{
    if (x >= 0 && x < VGA_COLS)
        cursor_x = x;
    if (y >= 0 && y < VGA_ROWS)
        cursor_y = y;
    update_cursor();
}

void vga_get_cursor(int* x, int* y)
{
    if (x) *x = cursor_x;
    if (y) *y = cursor_y;
}

void vga_backspace(void)
{
    if (cursor_x > 0) {
        cursor_x--;
    } else if (cursor_y > 0) {
        cursor_y--;
        cursor_x = VGA_COLS - 1;
    }
    vga_buffer[cursor_y * VGA_COLS + cursor_x] = (uint16_t)text_attr << 8 | ' ';
    update_cursor();
}

void terminal_clear(void)
{
    vga_clear();
}

/* ---- 增强版printf实现 ---- */

static void print_unsigned_padded(uint64_t val, int base, int uppercase, int width, int zero_pad)
{
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char* digits = uppercase ? digits_upper : digits_lower;
    char buf[65];
    int i = 0;
    char pad_char = zero_pad ? '0' : ' ';

    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val > 0) {
            buf[i++] = digits[val % base];
            val /= base;
        }
    }

    /* 填充到指定宽度 */
    int num_digits = i;
    while (num_digits < width) {
        vga_putchar(pad_char);
        num_digits++;
    }

    /* 逆序输出数字 */
    while (--i >= 0) {
        vga_putchar(buf[i]);
    }
}

static void print_signed_padded(int64_t val, int width, int zero_pad)
{
    if (val < 0) {
        vga_putchar('-');
        val = -val;
        if (width > 0) width--;
    }
    print_unsigned_padded((uint64_t)val, 10, 0, width, zero_pad);
}

int printf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    int count = 0;

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;

            /* 解析标志 */
            int zero_pad = 0;
            if (*fmt == '0') {
                zero_pad = 1;
                fmt++;
            }

            /* 解析宽度 */
            int width = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }

            /* 解析长度修饰符 */
            int long_long = 0;
            if (*fmt == 'l') {
                fmt++;
                if (*fmt == 'l') {
                    long_long = 1;
                    fmt++;
                }
            }

            switch (*fmt) {
            case 's': {
                const char* s = va_arg(args, const char*);
                if (!s) s = "(null)";
                while (*s) {
                    vga_putchar(*s);
                    s++;
                    count++;
                }
                break;
            }
            case 'd': {
                if (long_long) {
                    long long val = va_arg(args, long long);
                    print_signed_padded(val, width, zero_pad);
                } else {
                    int val = va_arg(args, int);
                    print_signed_padded(val, width, zero_pad);
                }
                count++;
                break;
            }
            case 'u': {
                if (long_long) {
                    unsigned long long val = va_arg(args, unsigned long long);
                    print_unsigned_padded(val, 10, 0, width, zero_pad);
                } else {
                    unsigned int val = va_arg(args, unsigned int);
                    print_unsigned_padded((uint64_t)val, 10, 0, width, zero_pad);
                }
                count++;
                break;
            }
            case 'x': {
                if (long_long) {
                    unsigned long long val = va_arg(args, unsigned long long);
                    print_unsigned_padded(val, 16, 0, width, zero_pad);
                } else {
                    unsigned int val = va_arg(args, unsigned int);
                    print_unsigned_padded((uint64_t)val, 16, 0, width, zero_pad);
                }
                count++;
                break;
            }
            case 'X': {
                if (long_long) {
                    unsigned long long val = va_arg(args, unsigned long long);
                    print_unsigned_padded(val, 16, 1, width, zero_pad);
                } else {
                    unsigned int val = va_arg(args, unsigned int);
                    print_unsigned_padded((uint64_t)val, 16, 1, width, zero_pad);
                }
                count++;
                break;
            }
            case 'p': {
                uintptr_t val = (uintptr_t)va_arg(args, void*);
                vga_putchar('0');
                vga_putchar('x');
                print_unsigned_padded((uint64_t)val, 16, 1, sizeof(uintptr_t) * 2, 1);
                count++;
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                vga_putchar(c);
                count++;
                break;
            }
            case '%':
                vga_putchar('%');
                count++;
                break;
            default:
                vga_putchar('%');
                vga_putchar(*fmt);
                count += 2;
                break;
            }
        } else {
            vga_putchar(*fmt);
            count++;
        }
        fmt++;
    }

    va_end(args);
    return count;
}
