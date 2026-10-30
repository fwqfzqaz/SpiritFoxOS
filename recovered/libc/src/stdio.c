/* SpiritFoxOS libc - Standard I/O implementation
 *
 * Phase 1: No buffering — all I/O goes directly through syscalls.
 * This is simple and correct; buffering can be added later.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include "internal.h"

/* ========================================================================
 * Standard streams — static FILE objects for fd 0/1/2
 * ======================================================================== */

static FILE _stdin_file  = { .fd = 0, .flags = _IO_READ,  .err = 0, .eof = 0 };
static FILE _stdout_file = { .fd = 1, .flags = _IO_WRITE, .err = 0, .eof = 0 };
static FILE _stderr_file = { .fd = 2, .flags = _IO_WRITE, .err = 0, .eof = 0 };

FILE *stdin  = &_stdin_file;
FILE *stdout = &_stdout_file;
FILE *stderr = &_stderr_file;

/* ========================================================================
 * File open / close
 * ======================================================================== */

FILE *fopen(const char *pathname, const char *mode)
{
    int flags = 0;
    if (strcmp(mode, "r") == 0 || strcmp(mode, "rb") == 0)
        flags = O_RDONLY;
    else if (strcmp(mode, "w") == 0 || strcmp(mode, "wb") == 0)
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (strcmp(mode, "a") == 0 || strcmp(mode, "ab") == 0)
        flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (strcmp(mode, "r+") == 0 || strcmp(mode, "rb+") == 0 || strcmp(mode, "r+b") == 0)
        flags = O_RDWR;
    else if (strcmp(mode, "w+") == 0 || strcmp(mode, "wb+") == 0 || strcmp(mode, "w+b") == 0)
        flags = O_RDWR | O_CREAT | O_TRUNC;
    else if (strcmp(mode, "a+") == 0 || strcmp(mode, "ab+") == 0 || strcmp(mode, "a+b") == 0)
        flags = O_RDWR | O_CREAT | O_APPEND;
    else
        return NULL;

    int fd = open(pathname, flags, 0666);
    if (fd < 0)
        return NULL;

    FILE *f = (FILE *)calloc(1, sizeof(FILE));
    if (!f) { close(fd); return NULL; }
    f->fd = fd;
    if (flags & O_RDONLY) f->flags = _IO_READ;
    if (flags & O_WRONLY) f->flags = _IO_WRITE;
    if (flags & O_RDWR)   f->flags = _IO_READ | _IO_WRITE;
    if (flags & O_APPEND) f->flags |= _IO_APPEND;
    return f;
}

FILE *fdopen(int fd, const char *mode)
{
    (void)mode;
    FILE *f = (FILE *)calloc(1, sizeof(FILE));
    if (!f) return NULL;
    f->fd = fd;
    f->flags = _IO_READ | _IO_WRITE;
    return f;
}

FILE *freopen(const char *pathname, const char *mode, FILE *stream)
{
    if (!stream) return NULL;
    fclose(stream);
    if (!pathname) return NULL;
    return fopen(pathname, mode);
}

int fclose(FILE *stream)
{
    if (!stream) return EOF;
    int ret = close(stream->fd);
    /* Don't free the static stdin/stdout/stderr */
    if (stream != &_stdin_file && stream != &_stdout_file && stream != &_stderr_file)
        free(stream);
    return ret < 0 ? EOF : 0;
}

/* ========================================================================
 * Read / Write
 * ======================================================================== */

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    ssize_t r = read(stream->fd, ptr, total);
    if (r < 0) { stream->err = 1; return 0; }
    if (r == 0) { stream->eof = 1; return 0; }
    return (size_t)r / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    ssize_t r = write(stream->fd, ptr, total);
    if (r < 0) { stream->err = 1; return 0; }
    return (size_t)r / size;
}

/* ========================================================================
 * Character I/O
 * ======================================================================== */

int fgetc(FILE *stream)
{
    unsigned char c;
    ssize_t r = read(stream->fd, &c, 1);
    if (r <= 0) {
        stream->eof = (r == 0);
        stream->err = (r < 0);
        return EOF;
    }
    return c;
}

int fputc(int c, FILE *stream)
{
    unsigned char ch = (unsigned char)c;
    ssize_t r = write(stream->fd, &ch, 1);
    if (r <= 0) { stream->err = 1; return EOF; }
    return (unsigned char)c;
}

char *fgets(char *s, int size, FILE *stream)
{
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}

int fputs(const char *s, FILE *stream)
{
    size_t len = strlen(s);
    ssize_t r = write(stream->fd, s, len);
    return (r < 0) ? EOF : 0;
}

int ungetc(int c, FILE *stream)
{
    /* Phase 1: no buffering, so ungetc is not properly supported.
     * Return EOF to indicate failure. */
    (void)c; (void)stream;
    return EOF;
}

/* ========================================================================
 * File positioning
 * ======================================================================== */

long ftell(FILE *stream)
{
    return (long)lseek(stream->fd, 0, SEEK_CUR);
}

int fseek(FILE *stream, long offset, int whence)
{
    off_t r = lseek(stream->fd, offset, whence);
    if (r < 0) return -1;
    stream->eof = 0;
    return 0;
}

void rewind(FILE *stream)
{
    fseek(stream, 0, SEEK_SET);
    stream->err = 0;
}

/* ========================================================================
 * Status
 * ======================================================================== */

int feof(FILE *stream)     { return stream->eof; }
int ferror(FILE *stream)   { return stream->err; }
void clearerr(FILE *stream){ stream->err = 0; stream->eof = 0; }
int fflush(FILE *stream)   { (void)stream; return 0; }  /* No buffering, no-op */

/* ========================================================================
 * Error reporting
 * ======================================================================== */

void perror(const char *s)
{
    if (s && *s) {
        write(STDERR_FILENO, s, strlen(s));
        write(STDERR_FILENO, ": ", 2);
    }
    const char *msg = "Unknown error";
    static const char *errlist[] = {
        [0]  = "Success",
        [1]  = "Operation not permitted",
        [2]  = "No such file or directory",
        [3]  = "No such process",
        [4]  = "Interrupted system call",
        [5]  = "I/O error",
        [6]  = "No such device or address",
        [9]  = "Bad file descriptor",
        [10] = "No child processes",
        [11] = "Resource temporarily unavailable",
        [12] = "Cannot allocate memory",
        [13] = "Permission denied",
        [14] = "Bad address",
        [16] = "Device or resource busy",
        [17] = "File exists",
        [20] = "Not a directory",
        [21] = "Is a directory",
        [22] = "Invalid argument",
        [28] = "No space left on device",
        [38] = "Function not implemented",
    };
    if (sfk_errno >= 0 && sfk_errno < (int)(sizeof(errlist) / sizeof(errlist[0])) && errlist[sfk_errno])
        msg = errlist[sfk_errno];
    write(STDERR_FILENO, msg, strlen(msg));
    write(STDERR_FILENO, "\n", 1);
}

/* ========================================================================
 * Formatted output — vsnprintf core implementation
 * ======================================================================== */

static int vsnprintf_internal(char *buf, size_t size, const char *fmt, va_list ap)
{
    size_t pos = 0;

    while (*fmt && pos < size - 1) {
        if (*fmt != '%') {
            buf[pos++] = *fmt++;
            continue;
        }
        fmt++;  /* skip '%' */

        /* Handle field width (simple: just skip digits) */
        int width = 0;
        int pad_zero = 0;
        if (*fmt == '0') { pad_zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Handle length modifier (l, ll) */
        int long_val = 0;
        if (*fmt == 'l') { long_val = 1; fmt++; }
        if (*fmt == 'l') { long_val = 2; fmt++; }

        switch (*fmt) {
        case 'd': case 'i': {
            long val;
            if (long_val >= 2) val = va_arg(ap, long);
            else if (long_val == 1) val = va_arg(ap, long);
            else val = va_arg(ap, int);
            if (val < 0) {
                if (pos < size - 1) buf[pos++] = '-';
                val = -val;
            }
            char tmp[20];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = '0' + val % 10; val /= 10; }
            /* Padding */
            while (len < width && pos < size - 1) {
                buf[pos++] = pad_zero ? '0' : ' ';
                width--;
            }
            for (int i = len - 1; i >= 0 && pos < size - 1; i--)
                buf[pos++] = tmp[i];
            break;
        }
        case 'u': {
            unsigned long val;
            if (long_val >= 2) val = va_arg(ap, unsigned long);
            else if (long_val == 1) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            char tmp[20];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = '0' + val % 10; val /= 10; }
            while (len < width && pos < size - 1) {
                buf[pos++] = pad_zero ? '0' : ' ';
                width--;
            }
            for (int i = len - 1; i >= 0 && pos < size - 1; i--)
                buf[pos++] = tmp[i];
            break;
        }
        case 'x': {
            unsigned long val;
            if (long_val >= 2) val = va_arg(ap, unsigned long);
            else if (long_val == 1) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            char tmp[16];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = "0123456789abcdef"[val % 16]; val /= 16; }
            while (len < width && pos < size - 1) {
                buf[pos++] = pad_zero ? '0' : ' ';
                width--;
            }
            for (int i = len - 1; i >= 0 && pos < size - 1; i--)
                buf[pos++] = tmp[i];
            break;
        }
        case 'X': {
            unsigned long val;
            if (long_val >= 2) val = va_arg(ap, unsigned long);
            else if (long_val == 1) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            char tmp[16];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = "0123456789ABCDEF"[val % 16]; val /= 16; }
            while (len < width && pos < size - 1) {
                buf[pos++] = pad_zero ? '0' : ' ';
                width--;
            }
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
            int slen = 0;
            while (s[slen]) slen++;
            while (slen < width && pos < size - 1) {
                buf[pos++] = ' ';
                width--;
            }
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

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
    return vsnprintf_internal(str, size, fmt, ap);
}

int snprintf(char *str, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf_internal(str, size, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *str, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf_internal(str, (size_t)-1, fmt, ap);
    va_end(ap);
    return ret;
}

int printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf_internal(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0)
        write(STDOUT_FILENO, buf, (size_t)len);
    return len;
}

int fprintf(FILE *stream, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf_internal(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0)
        write(stream->fd, buf, (size_t)len);
    return len;
}

/* ========================================================================
 * Simple output
 * ======================================================================== */

int puts(const char *s)
{
    size_t len = strlen(s);
    write(STDOUT_FILENO, s, len);
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}

int putchar(int c)
{
    char ch = (char)c;
    write(STDOUT_FILENO, &ch, 1);
    return c;
}
