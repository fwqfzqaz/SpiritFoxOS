#ifndef _STDDEF_H
#define _STDDEF_H

#include <stdint.h>

typedef uint64_t size_t;
typedef int64_t  ssize_t;
typedef int64_t  ptrdiff_t;
typedef uint64_t uintptr_t;
typedef int64_t  intptr_t;

#define NULL ((void *)0)

#endif
