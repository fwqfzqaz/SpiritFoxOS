/* SpiritFoxOS libc - String functions */

#include <string.h>
#include <stddef.h>
#include <stdlib.h>

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
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

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    while (n--) {
        if (*p1 != *p2)
            return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    while (n--) {
        if (*p == (unsigned char)c)
            return (void *)p;
        p++;
    }
    return NULL;
}

size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len])
        len++;
    return len;
}

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++))
        ;
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    char *d = dest;
    while (n-- && (*d++ = *src++))
        ;
    while (n--)
        *d++ = '\0';
    return dest;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n-- && *s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    if (n == (size_t)-1)
        return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char *strcat(char *dest, const char *src)
{
    char *d = dest;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dest;
}

char *strncat(char *dest, const char *src, size_t n)
{
    char *d = dest;
    while (*d)
        d++;
    while (n-- && *src)
        *d++ = *src++;
    *d = '\0';
    return dest;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return c == 0 ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c)
            last = s;
        s++;
    }
    return c == 0 ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0)
        return (char *)haystack;
    while (*haystack) {
        if (*haystack == *needle && memcmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t len = 0;
    while (*s) {
        const char *a = accept;
        while (*a && *a != *s)
            a++;
        if (!*a)
            break;
        len++;
        s++;
    }
    return len;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t len = 0;
    while (*s) {
        const char *r = reject;
        while (*r && *r != *s)
            r++;
        if (*r)
            break;
        len++;
        s++;
    }
    return len;
}

char *strpbrk(const char *s, const char *accept)
{
    while (*s) {
        const char *a = accept;
        while (*a) {
            if (*a == *s)
                return (char *)s;
            a++;
        }
        s++;
    }
    return NULL;
}

char *strtok(char *str, const char *delim)
{
    static char *next = NULL;
    if (str)
        next = str;
    if (!next)
        return NULL;

    /* Skip leading delimiters */
    next += strspn(next, delim);
    if (!*next) {
        next = NULL;
        return NULL;
    }

    char *token = next;
    next += strcspn(next, delim);
    if (*next)
        *next++ = '\0';
    else
        next = NULL;
    return token;
}

char *strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *d = (char *)malloc(len);
    if (d)
        memcpy(d, s, len);
    return d;
}
