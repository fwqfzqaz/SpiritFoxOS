#ifndef SF_POSIX_IMPORTS_H
#define SF_POSIX_IMPORTS_H
#include "posix.h"
/* Application-side import slots. sfpack resolves each slot symbol to a writable
 * eight-byte pointer and emits one import relocation record per entry. The
 * loader fills these slots with addresses of functions exported by the
 * authenticated runtime library in the same address space. */
extern int (*__sf_import_open)(const char *,int,...);
extern long (*__sf_import_read)(int,void *,unsigned long);
extern long (*__sf_import_write)(int,const void *,unsigned long);
extern int (*__sf_import_close)(int);
extern long (*__sf_import_lseek)(int,long,int);
extern int (*__sf_import_fstat)(int,struct stat *);
extern int (*__sf_import_stat)(const char *,struct stat *);
extern int (*__sf_import_getpid)(void);
extern void (*__sf_import__exit)(int);
extern unsigned (*__sf_import_sleep)(unsigned);
extern int (*__sf_import_printf)(const char *, ...);
extern int (*__sf_import_puts)(const char *);
extern char *(*__sf_import_getenv)(const char *);
extern int *(*__sf_import___errno_location)(void);
extern const char *(*__sf_import_strerror)(int);

#define open(...) __sf_import_open(__VA_ARGS__)
#define read(...) __sf_import_read(__VA_ARGS__)
#define write(...) __sf_import_write(__VA_ARGS__)
#define close(...) __sf_import_close(__VA_ARGS__)
#define lseek(...) __sf_import_lseek(__VA_ARGS__)
#define fstat(...) __sf_import_fstat(__VA_ARGS__)
#define stat(...) __sf_import_stat(__VA_ARGS__)
#define getpid(...) __sf_import_getpid(__VA_ARGS__)
#define _exit(...) __sf_import__exit(__VA_ARGS__)
#define sleep(...) __sf_import_sleep(__VA_ARGS__)
#define printf(...) __sf_import_printf(__VA_ARGS__)
#define puts(...) __sf_import_puts(__VA_ARGS__)
#define getenv(...) __sf_import_getenv(__VA_ARGS__)
#define strerror(...) __sf_import_strerror(__VA_ARGS__)
#define __errno_location(...) __sf_import___errno_location(__VA_ARGS__)
#endif
