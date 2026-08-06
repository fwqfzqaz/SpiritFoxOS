#include "string.h"

size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *(unsigned char*)a - *(unsigned char*)b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++))
        ;
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    char* d = dest;
    while (n > 0 && *src) {
        *d++ = *src++;
        n--;
    }
    while (n > 0) {
        *d++ = '\0';
        n--;
    }
    return dest;
}

char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++))
        ;
    return dest;
}

void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    /* 小数据量直接逐字节填充 */
    if (n < 16) {
        while (n--)
            *p++ = (unsigned char)c;
        return s;
    }
    /* 对齐到 8 字节边界 */
    size_t head = (size_t)p & 7;
    if (head) {
        head = 8 - head;
        if (head > n) head = n;
        for (size_t i = 0; i < head; i++)
            p[i] = (unsigned char)c;
        p += head;
        n -= head;
    }
    /* 64 位批量填充 (rep stosq，每次写 8 字节) */
    uint64_t c64 = (uint8_t)c;
    c64 |= c64 << 8;
    c64 |= c64 << 16;
    c64 |= c64 << 32;
    size_t qwords = n >> 3;
    if (qwords) {
        __asm__ volatile ("rep stosq"
            : "+D"(p), "+c"(qwords)
            : "a"(c64)
            : "memory");
    }
    /* 尾部不足 8 字节 */
    n &= 7;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    /* 小数据量直接逐字节拷贝 */
    if (n < 16) {
        while (n--)
            *d++ = *s++;
        return dest;
    }
    /* 对齐目标地址到 8 字节边界 */
    size_t head = (size_t)d & 7;
    if (head) {
        head = 8 - head;
        if (head > n) head = n;
        for (size_t i = 0; i < head; i++)
            d[i] = s[i];
        d += head;
        s += head;
        n -= head;
    }
    /* 64 位批量拷贝 (rep movsq，每次拷 8 字节) */
    size_t qwords = n >> 3;
    if (qwords) {
        __asm__ volatile ("rep movsq"
            : "+S"(s), "+D"(d), "+c"(qwords)
            :
            : "memory");
    }
    /* 尾部不足 8 字节 */
    n &= 7;
    while (n--)
        *d++ = *s++;
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0)
        return dest;

    if (d < s) {
        /* 正向拷贝：对齐到 8 字节后用 rep movsq */
        size_t head = (size_t)d & 7;
        if (head) {
            head = 8 - head;
            if (head > n) head = n;
            for (size_t i = 0; i < head; i++)
                d[i] = s[i];
            d += head;
            s += head;
            n -= head;
        }
        size_t qwords = n >> 3;
        if (qwords) {
            __asm__ volatile ("rep movsq"
                : "+S"(s), "+D"(d), "+c"(qwords)
                :
                : "memory");
        }
        n &= 7;
        while (n--)
            *d++ = *s++;
    } else {
        /* 反向拷贝：先处理尾部对齐，再 64 位反向拷贝，最后处理头部 */
        d += n;
        s += n;
        /* 尾部字节使目标 8 字节对齐 */
        size_t tail = (size_t)d & 7;
        if (tail) {
            if (tail > n) tail = n;
            for (size_t i = 0; i < tail; i++)
                *--d = *--s;
            n -= tail;
        }
        /* 64 位反向拷贝 */
        size_t qwords = n >> 3;
        while (qwords--) {
            d -= 8;
            s -= 8;
            *(uint64_t*)d = *(const uint64_t*)s;
        }
        /* 剩余头部字节 */
        n &= 7;
        while (n--)
            *--d = *--s;
    }
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* a = (const unsigned char*)s1;
    const unsigned char* b = (const unsigned char*)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return a[i] - b[i];
    }
    return 0;
}

static char* strtok_next = (void*)0;

char* strtok(char* str, const char* delim) {
    if (str == (void*)0)
        str = strtok_next;
    if (str == (void*)0)
        return (void*)0;

    /* skip leading delimiters */
    while (*str) {
        const char* d = delim;
        int is_delim = 0;
        while (*d) {
            if (*str == *d) { is_delim = 1; break; }
            d++;
        }
        if (!is_delim) break;
        str++;
    }

    if (*str == '\0') {
        strtok_next = (void*)0;
        return (void*)0;
    }

    char* token_start = str;

    /* find end of token */
    while (*str) {
        const char* d = delim;
        int is_delim = 0;
        while (*d) {
            if (*str == *d) { is_delim = 1; break; }
            d++;
        }
        if (is_delim) {
            *str = '\0';
            strtok_next = str + 1;
            return token_start;
        }
        str++;
    }

    strtok_next = (void*)0;
    return token_start;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c)
            return (char*)s;
        s++;
    }
    if (c == 0)
        return (char*)s;
    return (void*)0;
}

int atoi(const char* s) {
    int result = 0;
    int sign = 1;

    while (*s == ' ' || *s == '\t') s++;

    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }

    return sign * result;
}

char* itoa(int value, char* buf, int base)
{
    char *p = buf;
    char *p1, *p2;
    unsigned int ud = value;
    int divisor = 10;

    if (base == 'd' || base == 10) {
        if (value < 0) {
            *p++ = '-';
            ud = -value;
        }
    } else if (base == 'x' || base == 16) {
        divisor = 16;
    } else if (base == 'o' || base == 8) {
        divisor = 8;
    } else if (base == 'b' || base == 2) {
        divisor = 2;
    } else {
        divisor = base;
    }

    /* Divide UD by DIVISOR until UD == 0 */
    do {
        int remainder = ud % divisor;
        *p++ = (remainder < 10) ? remainder + '0' : remainder + 'a' - 10;
    } while (ud /= divisor);

    /* Terminate BUF */
    *p = '\0';

    /* Reverse the string (but not the '-' prefix) */
    p1 = (buf[0] == '-') ? buf + 1 : buf;
    p2 = p - 1;
    while (p1 < p2) {
        char tmp = *p1;
        *p1++ = *p2;
        *p2-- = tmp;
    }

    return buf;
}

char* utoa(unsigned int value, char* buf, int base)
{
    char *p = buf;
    char *p1, *p2;
    unsigned int ud = value;
    int divisor = 10;

    if (base == 'x' || base == 16) {
        divisor = 16;
    } else if (base == 'o' || base == 8) {
        divisor = 8;
    } else if (base == 'b' || base == 2) {
        divisor = 2;
    } else {
        divisor = base;
    }

    do {
        int remainder = ud % divisor;
        *p++ = (remainder < 10) ? remainder + '0' : remainder + 'a' - 10;
    } while (ud /= divisor);

    *p = '\0';

    p1 = buf;
    p2 = p - 1;
    while (p1 < p2) {
        char tmp = *p1;
        *p1++ = *p2;
        *p2-- = tmp;
    }

    return buf;
}
