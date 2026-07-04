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
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (n--)
        *d++ = *s++;
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
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
