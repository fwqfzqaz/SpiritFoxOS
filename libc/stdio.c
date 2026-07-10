/* SpiritFoxOS libc - Minimal stdio implementation */

#include <stdarg.h>
#include <stddef.h>

/* Syscall wrappers (defined in syscall.c) */
extern int64_t sfk_syscall3(int64_t num, int64_t a1, int64_t a2, int64_t a3);
extern int64_t sfk_syscall1(int64_t num, int64_t a1);

#define SYS_write 1

#define STDOUT_FILENO 1
#define STDERR_FILENO 2

static int puts_fd(int fd, const char *s)
{
    size_t len = 0;
    while (s[len]) len++;
    int64_t ret = sfk_syscall3(SYS_write, fd, (int64_t)s, len);
    return (ret < 0) ? -1 : (int)ret;
}

int puts(const char *s)
{
    int ret = puts_fd(STDOUT_FILENO, s);
    puts_fd(STDOUT_FILENO, "\n");
    return ret;
}

int putchar(int c)
{
    char buf[1] = { (char)c };
    int64_t ret = sfk_syscall3(SYS_write, STDOUT_FILENO, (int64_t)buf, 1);
    return (ret < 0) ? -1 : c;
}

static int vsnprintf_internal(char *buf, size_t size, const char *fmt, va_list ap)
{
    size_t pos = 0;
    
    while (*fmt && pos < size - 1) {
        if (*fmt != '%') {
            buf[pos++] = *fmt++;
            continue;
        }
        fmt++;  /* skip '%' */
        
        switch (*fmt) {
        case 'd': case 'i': {
            int val = va_arg(ap, int);
            if (val < 0) {
                if (pos < size - 1) buf[pos++] = '-';
                val = -val;
            }
            char tmp[20];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = '0' + val % 10; val /= 10; }
            for (int i = len - 1; i >= 0 && pos < size - 1; i--)
                buf[pos++] = tmp[i];
            break;
        }
        case 'u': {
            unsigned int val = va_arg(ap, unsigned int);
            char tmp[20];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = '0' + val % 10; val /= 10; }
            for (int i = len - 1; i >= 0 && pos < size - 1; i--)
                buf[pos++] = tmp[i];
            break;
        }
        case 'x': {
            unsigned int val = va_arg(ap, unsigned int);
            char tmp[16];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = "0123456789abcdef"[val % 16]; val /= 16; }
            for (int i = len - 1; i >= 0 && pos < size - 1; i--)
                buf[pos++] = tmp[i];
            break;
        }
        case 'p': {
            unsigned long val = (unsigned long)va_arg(ap, void *);
            if (pos + 2 < size) { buf[pos++] = '0'; buf[pos++] = 'x'; }
            char tmp[16];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = "0123456789abcdef"[val % 16]; val /= 16; }
            for (int i = len - 1; i >= 0 && pos < size - 1; i--)
                buf[pos++] = tmp[i];
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && pos < size - 1)
                buf[pos++] = *s++;
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (pos < size - 1) buf[pos++] = c;
            break;
        }
        case '%':
            if (pos < size - 1) buf[pos++] = '%';
            break;
        default:
            if (pos < size - 1) buf[pos++] = '%';
            if (pos < size - 1) buf[pos++] = *fmt;
            break;
        }
        fmt++;
    }
    buf[pos] = '\0';
    return (int)pos;
}

int printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf_internal(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0) {
        int64_t ret = sfk_syscall3(SYS_write, STDOUT_FILENO, (int64_t)buf, len);
        if (ret < 0) return -1;
    }
    return len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf_internal(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}
