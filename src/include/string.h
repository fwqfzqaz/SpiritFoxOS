/* SpiritFoxOS - 字符串操作函数
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
#ifndef STRING_H
#define STRING_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

static inline void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

static inline void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

static inline void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

static inline int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t *)s1;
    const uint8_t *b = (const uint8_t *)s2;
    while (n--) {
        if (*a != *b) return *a - *b;
        a++; b++;
    }
    return 0;
}

static inline size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static inline char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

static inline char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n-- && (*d++ = *src++));
    while (n-- > 0) *d++ = '\0';
    return dest;
}

static inline int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(const uint8_t *)s1 - *(const uint8_t *)s2;
}

static inline int strncmp(const char *s1, const char *s2, size_t n) {
    while (n-- && *s1 && *s1 == *s2) { s1++; s2++; }
    if (n == (size_t)-1) return 0;
    return *(const uint8_t *)s1 - *(const uint8_t *)s2;
}

/* 跳过前导空白字符 */
static inline const char *skip_spaces(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* 辅助函数：按指定进制写入无符号整数，支持零填充 */
static inline int snprintf_write_uint(char *buf, size_t size, int pos,
                                       uint32_t v, int base, int width, int zeropad) {
    const char digits[] = "0123456789abcdef";
    char nb[12];
    int ni = 0;
    if (v == 0) {
        nb[ni++] = '0';
    } else {
        while (v > 0) {
            nb[ni++] = digits[v % base];
            v /= base;
        }
    }
    /* 如需要则用零或空格填充 */
    int pad = width - ni;
    if (pad > 0) {
        for (int p = 0; p < pad && (size_t)pos < size - 1; p++) {
            buf[pos++] = zeropad ? '0' : ' ';
        }
    }
    while (--ni >= 0 && (size_t)pos < size - 1) {
        buf[pos++] = nb[ni];
    }
    return pos;
}

/* 简易 snprintf - 支持 %s、%d、%u、%x、%c、%p、%% 以及宽度格式如 %02x */
static inline int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int pos = 0;
    while (*fmt && (size_t)pos < size - 1) {
        if (*fmt == '%') {
            fmt++;
            /* 解析宽度和零填充标志 */
            int zeropad = 0;
            int width = 0;
            if (*fmt == '0') { zeropad = 1; fmt++; }
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
            if (*fmt == 's') {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s && (size_t)pos < size - 1) buf[pos++] = *s++;
            } else if (*fmt == 'd') {
                int v = va_arg(args, int);
                if (v < 0) { buf[pos++] = '-'; v = -v; }
                pos = snprintf_write_uint(buf, size, pos, (uint32_t)v, 10, width, zeropad);
            } else if (*fmt == 'u') {
                uint32_t v = (uint32_t)va_arg(args, uint64_t);
                pos = snprintf_write_uint(buf, size, pos, v, 10, width, zeropad);
            } else if (*fmt == 'x') {
                uint32_t v = (uint32_t)va_arg(args, uint64_t);
                pos = snprintf_write_uint(buf, size, pos, v, 16, width, zeropad);
            } else if (*fmt == 'p') {
                buf[pos++] = '0';
                if ((size_t)pos < size - 1) buf[pos++] = 'x';
                uint64_t v = (uint64_t)va_arg(args, void *);
                const char hx[] = "0123456789abcdef";
                char nb[17]; int ni = 0;
                if (v == 0) { nb[ni++] = '0'; }
                else { while (v > 0) { nb[ni++] = hx[v & 0xF]; v >>= 4; } }
                while (ni < 16 && (size_t)pos < size - 1) buf[pos++] = '0';
                while (--ni >= 0 && (size_t)pos < size - 1) buf[pos++] = nb[ni];
            } else if (*fmt == 'c') {
                buf[pos++] = (char)va_arg(args, int);
            } else if (*fmt == '%') {
                buf[pos++] = '%';
            } else {
                buf[pos++] = '%';
                if ((size_t)pos < size - 1) buf[pos++] = *fmt;
            }
        } else {
            buf[pos++] = *fmt;
        }
        fmt++;
    }
    buf[pos] = '\0';
    va_end(args);
    return pos;
}

#endif /* STRING_H */
