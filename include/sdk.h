#ifndef SF_SDK_H
#define SF_SDK_H
#include <stdarg.h>
#include "sf.h"
/* Bounded user-space SDK helpers adapted from the pre-refactor libc.
 * Design constraints: no syscalls, no malloc, no global state, no streams.
 * Every function is deterministic and safe for untrusted lengths; the
 * formatting family never writes past size-1 and returns the full length. */
void *sf_memmove(void *,const void *,size_t);
void *sf_memchr(const void *,int,size_t);
int sf_strncmp(const char *,const char *,size_t);
char *sf_strcpy(char *,const char *);
char *sf_strncpy(char *,const char *,size_t);
char *sf_strcat(char *,const char *);
char *sf_strncat(char *,const char *,size_t);
char *sf_strchr(const char *,int);
char *sf_strrchr(const char *,int);
char *sf_strstr(const char *,const char *);
size_t sf_strspn(const char *,const char *);
size_t sf_strcspn(const char *,const char *);
char *sf_strpbrk(const char *,const char *);
/* Reentrant tokenizer; the caller owns the cursor, as strtok_r. */
char *sf_strtok_r(char *,const char *,char **);
int sf_isalpha(int);
int sf_isdigit(int);
int sf_isalnum(int);
int sf_isspace(int);
int sf_isupper(int);
int sf_islower(int);
int sf_isprint(int);
int sf_ispunct(int);
int sf_isxdigit(int);
int sf_isgraph(int);
int sf_iscntrl(int);
int sf_toupper(int);
int sf_tolower(int);
int sf_abs(int);
long sf_labs(long);
int sf_atoi(const char *);
long sf_atol(const char *);
long sf_strtol(const char *,char **,int);
unsigned long sf_strtoul(const char *,char **,int);
void *sf_bsearch(const void *,const void *,size_t,size_t,int (*)(const void *,const void *));
int sf_vsnprintf(char *,size_t,const char *,va_list);
int sf_snprintf(char *,size_t,const char *,...);
#endif
