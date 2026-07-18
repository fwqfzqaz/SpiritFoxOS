/* SpiritFoxOS libc - Standard library functions */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "internal.h"

/* ========================================================================
 * atexit support
 * ======================================================================== */

#define ATEXIT_MAX 32
static void (*atexit_funcs[ATEXIT_MAX])(void);
static int atexit_count = 0;

int atexit(void (*func)(void))
{
    if (atexit_count >= ATEXIT_MAX)
        return -1;
    atexit_funcs[atexit_count++] = func;
    return 0;
}

/* ========================================================================
 * Process control
 * ======================================================================== */

void abort(void)
{
    _exit(134);  /* 128 + SIGABRT(6) */
    __builtin_unreachable();
}

void exit(int status)
{
    /* Call atexit handlers in reverse order */
    while (atexit_count > 0)
        atexit_funcs[--atexit_count]();
    sfk_syscall1(SYS_exit_group, status);
    __builtin_unreachable();
}

/* ========================================================================
 * Absolute value
 * ======================================================================== */

int abs(int j)
{
    return j < 0 ? -j : j;
}

long labs(long j)
{
    return j < 0 ? -j : j;
}

/* ========================================================================
 * Numeric conversion
 * ======================================================================== */

int atoi(const char *s)
{
    int n = 0, sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return sign * n;
}

long atol(const char *s)
{
    long n = 0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return sign * n;
}

long strtol(const char *nptr, char **endptr, int base)
{
    const char *s = nptr;
    long acc = 0;
    int neg = 0;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    if (base == 0) {
        if (*s == '0') {
            if (s[1] == 'x' || s[1] == 'X') { base = 16; s += 2; }
            else base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    while (*s) {
        int dig;
        if (*s >= '0' && *s <= '9') dig = *s - '0';
        else if (*s >= 'a' && *s <= 'z') dig = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') dig = *s - 'A' + 10;
        else break;
        if (dig >= base) break;
        acc = acc * base + dig;
        s++;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -acc : acc;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    const char *s = nptr;
    unsigned long acc = 0;
    int neg = 0;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    if (base == 0) {
        if (*s == '0') {
            if (s[1] == 'x' || s[1] == 'X') { base = 16; s += 2; }
            else base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    while (*s) {
        int dig;
        if (*s >= '0' && *s <= '9') dig = *s - '0';
        else if (*s >= 'a' && *s <= 'z') dig = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') dig = *s - 'A' + 10;
        else break;
        if (dig >= base) break;
        acc = acc * base + dig;
        s++;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -acc : acc;
}

/* ========================================================================
 * Random numbers (simple LCG)
 * ======================================================================== */

static unsigned long rand_seed = 1;

int rand(void)
{
    rand_seed = rand_seed * 1103515245UL + 12345UL;
    return (int)((rand_seed >> 16) & 0x7fff);
}

void srand(unsigned int seed)
{
    rand_seed = seed;
}

/* ========================================================================
 * Environment variables (Phase 1: stub)
 * ======================================================================== */

static char *__environ[1] = { NULL };
char **environ = __environ;

char *getenv(const char *name)
{
    (void)name;
    /* Phase 1: no environment variable support */
    if (!environ) return NULL;
    size_t len = strlen(name);
    for (char **ep = environ; *ep; ep++) {
        if (strncmp(*ep, name, len) == 0 && (*ep)[len] == '=')
            return *ep + len + 1;
    }
    return NULL;
}

int setenv(const char *name, const char *value, int overwrite)
{
    (void)name; (void)value; (void)overwrite;
    return -1;  /* Phase 1: not supported */
}

int unsetenv(const char *name)
{
    (void)name;
    return -1;
}

/* ========================================================================
 * Sorting and searching
 * ======================================================================== */

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    /* Insertion sort — simple and adequate for small arrays */
    char *arr = (char *)base;
    char *tmp = (char *)malloc(size);
    if (!tmp) return;

    for (size_t i = 1; i < nmemb; i++) {
        size_t j = i;
        while (j > 0 && compar(arr + (j - 1) * size, arr + j * size) > 0) {
            memcpy(tmp, arr + j * size, size);
            memcpy(arr + j * size, arr + (j - 1) * size, size);
            memcpy(arr + (j - 1) * size, tmp, size);
            j--;
        }
    }
    free(tmp);
}

void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size, int (*compar)(const void *, const void *))
{
    const char *arr = (const char *)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compar(key, arr + mid * size);
        if (cmp == 0) return (void *)(arr + mid * size);
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}
