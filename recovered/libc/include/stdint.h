#ifndef _STDINT_H
#define _STDINT_H

/* Exact-width integer types */
typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef signed short       int16_t;
typedef unsigned short     uint16_t;
typedef signed int         int32_t;
typedef unsigned int       uint32_t;
typedef signed long long   int64_t;
typedef unsigned long long uint64_t;

/* Minimum-width types */
typedef int8_t   int_least8_t;
typedef uint8_t  uint_least8_t;
typedef int16_t  int_least16_t;
typedef uint16_t uint_least16_t;
typedef int32_t  int_least32_t;
typedef uint32_t uint_least32_t;
typedef int64_t  int_least64_t;
typedef uint64_t uint_least64_t;

/* Fast types (use native word size on x86_64) */
typedef int64_t  int_fast8_t;
typedef uint64_t uint_fast8_t;
typedef int64_t  int_fast16_t;
typedef uint64_t uint_fast16_t;
typedef int64_t  int_fast32_t;
typedef uint64_t uint_fast32_t;
typedef int64_t  int_fast64_t;
typedef uint64_t uint_fast64_t;

/* Pointer-holding types */
typedef int64_t  intptr_t;
typedef uint64_t uintptr_t;

/* Maximum-width types */
typedef int64_t  intmax_t;
typedef uint64_t uintmax_t;

/* Limits for exact-width types */
#define INT8_MIN    (-1 - 0x7f)
#define INT8_MAX    0x7f
#define UINT8_MAX   0xff

#define INT16_MIN   (-1 - 0x7fff)
#define INT16_MAX   0x7fff
#define UINT16_MAX  0xffff

#define INT32_MIN   (-1 - 0x7fffffff)
#define INT32_MAX   0x7fffffff
#define UINT32_MAX  0xffffffffU

#define INT64_MIN   (-1 - 0x7fffffffffffffffLL)
#define INT64_MAX   0x7fffffffffffffffLL
#define UINT64_MAX  0xffffffffffffffffULL

/* Limits for minimum-width types */
#define INT_LEAST8_MIN    INT8_MIN
#define INT_LEAST8_MAX    INT8_MAX
#define UINT_LEAST8_MAX   UINT8_MAX
#define INT_LEAST16_MIN   INT16_MIN
#define INT_LEAST16_MAX   INT16_MAX
#define UINT_LEAST16_MAX  UINT16_MAX
#define INT_LEAST32_MIN   INT32_MIN
#define INT_LEAST32_MAX   INT32_MAX
#define UINT_LEAST32_MAX  UINT32_MAX
#define INT_LEAST64_MIN   INT64_MIN
#define INT_LEAST64_MAX   INT64_MAX
#define UINT_LEAST64_MAX  UINT64_MAX

/* Other limits */
#define SIZE_MAX    UINT64_MAX
#define PTRDIFF_MIN INT64_MIN
#define PTRDIFF_MAX INT64_MAX
#define INTPTR_MIN  INT64_MIN
#define INTPTR_MAX  INT64_MAX
#define UINTPTR_MAX UINT64_MAX
#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX

/* Printf/scanf format specifiers */
#define PRId8  "d"
#define PRIu8  "u"
#define PRId16 "d"
#define PRIu16 "u"
#define PRId32 "d"
#define PRIu32 "u"
#define PRId64 "lld"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRIX64 "llX"
#define PRIdPTR "ld"
#define PRIuPTR "lu"
#define PRIxPTR "lx"

#define SCNd8  "hhd"
#define SCNu8  "hhu"
#define SCNd16 "hd"
#define SCNu16 "hu"
#define SCNd32 "d"
#define SCNu32 "u"
#define SCNd64 "lld"
#define SCNu64 "llu"

#endif /* _STDINT_H */
