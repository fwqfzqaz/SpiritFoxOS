#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

/* Exit status constants */
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* Random number maximum */
#define RAND_MAX 2147483647

/* Process control */
void abort(void) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));
void _exit(int status) __attribute__((noreturn));
int atexit(void (*func)(void));

/* Numeric conversion */
int atoi(const char *s);
long atol(const char *s);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);

/* Absolute value */
int abs(int j);
long labs(long j);

/* Memory allocation */
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t num, size_t size);
void *realloc(void *ptr, size_t size);

/* Sorting / searching */
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size, int (*compar)(const void *, const void *));

/* Random numbers */
int rand(void);
void srand(unsigned int seed);

/* Environment variables */
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);

/* Environment pointer */
extern char **environ;

#endif /* _STDLIB_H */
