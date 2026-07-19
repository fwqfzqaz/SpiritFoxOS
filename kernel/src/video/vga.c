#include "vga.h"
#include "hal.h"
#include "fb.h"
#include "smp.h"
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

/* VGA 输出自旋锁（多核安全）
 * 保护 cursor_x/y 和 VGA 缓冲区的一致性。
 * 使用 irqsave 风格的票据锁，中断处理程序中也能安全使用。 */
static spinlock_t vga_lock;
static int vga_lock_initialized = 0;

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

    spinlock_init(&vga_lock);
    vga_lock_initialized = 1;

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
    /* 多核安全：在修改光标位置和VGA缓冲区前加锁 */
    int locked = 0;
    if (vga_lock_initialized) {
        spinlock_acquire(&vga_lock);
        locked = 1;
    }

    /* 如果帧缓冲终端激活，则渲染到帧缓冲而非VGA文本缓冲。
     * fb_term_putchar 会同时输出到串口，避免重复调用 serial_putchar。 */
    if (fb_term_is_active()) {
        if (locked) spinlock_release(&vga_lock);
        fb_term_putchar(c);
        return;
    }

    /* 非帧缓冲模式：同时输出到串口和VGA文本缓冲 */
    serial_putchar(c);

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

    if (locked) spinlock_release(&vga_lock);
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

static void print_unsigned_padded(uint64_t val, int base, int uppercase, int width, int zero_pad, int left_align)
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

    int num_digits = i;

    if (left_align) {
        /* 左对齐：先输出数字，再补空格 */
        while (--i >= 0) {
            vga_putchar(buf[i]);
        }
        while (num_digits < width) {
            vga_putchar(' ');
            num_digits++;
        }
    } else {
        /* 右对齐：先补位，再输出数字 */
        while (num_digits < width) {
            vga_putchar(pad_char);
            num_digits++;
        }
        while (--i >= 0) {
            vga_putchar(buf[i]);
        }
    }
}

static void print_signed_padded(int64_t val, int width, int zero_pad, int left_align)
{
    if (val < 0) {
        vga_putchar('-');
        val = -val;
        if (width > 0) width--;
    }
    print_unsigned_padded((uint64_t)val, 10, 0, width, zero_pad, left_align);
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
            int left_align = 0;
            if (*fmt == '-') {
                left_align = 1;
                fmt++;
            }
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
                int slen = 0;
                while (s[slen]) slen++;
                if (left_align) {
                    /* 左对齐：先输出字符串，再补空格 */
                    for (int i = 0; i < slen; i++) {
                        vga_putchar(s[i]);
                        count++;
                    }
                    for (int i = slen; i < width; i++) {
                        vga_putchar(' ');
                        count++;
                    }
                } else {
                    /* 右对齐：先补空格，再输出字符串 */
                    for (int i = slen; i < width; i++) {
                        vga_putchar(' ');
                        count++;
                    }
                    for (int i = 0; i < slen; i++) {
                        vga_putchar(s[i]);
                        count++;
                    }
                }
                break;
            }
            case 'd': {
                if (long_long) {
                    long long val = va_arg(args, long long);
                    print_signed_padded(val, width, zero_pad, left_align);
                } else {
                    int val = va_arg(args, int);
                    print_signed_padded(val, width, zero_pad, left_align);
                }
                count++;
                break;
            }
            case 'u': {
                if (long_long) {
                    unsigned long long val = va_arg(args, unsigned long long);
                    print_unsigned_padded(val, 10, 0, width, zero_pad, left_align);
                } else {
                    unsigned int val = va_arg(args, unsigned int);
                    print_unsigned_padded((uint64_t)val, 10, 0, width, zero_pad, left_align);
                }
                count++;
                break;
            }
            case 'x': {
                if (long_long) {
                    unsigned long long val = va_arg(args, unsigned long long);
                    print_unsigned_padded(val, 16, 0, width, zero_pad, left_align);
                } else {
                    unsigned int val = va_arg(args, unsigned int);
                    print_unsigned_padded((uint64_t)val, 16, 0, width, zero_pad, left_align);
                }
                count++;
                break;
            }
            case 'X': {
                if (long_long) {
                    unsigned long long val = va_arg(args, unsigned long long);
                    print_unsigned_padded(val, 16, 1, width, zero_pad, left_align);
                } else {
                    unsigned int val = va_arg(args, unsigned int);
                    print_unsigned_padded((uint64_t)val, 16, 1, width, zero_pad, left_align);
                }
                count++;
                break;
            }
            case 'p': {
                uintptr_t val = (uintptr_t)va_arg(args, void*);
                vga_putchar('0');
                vga_putchar('x');
                print_unsigned_padded((uint64_t)val, 16, 1, sizeof(uintptr_t) * 2, 1, 0);
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
