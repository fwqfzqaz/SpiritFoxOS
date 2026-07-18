#ifndef _STDDEF_H
#define _STDDEF_H

#include <stdint.h>

typedef uint64_t size_t;
typedef int64_t  ssize_t;
typedef int64_t  ptrdiff_t;
typedef uint64_t uintptr_t;
typedef int64_t  intptr_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef offsetof
#define offsetof(type, member) ((size_t) &((type *)0)->member)
#endif

#endif /* _STDDEF_H */
