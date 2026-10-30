#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>

/* Constants */
#define EOF         (-1)
#define BUFSIZ      8192
#define FILENAME_MAX 256
#define FOPEN_MAX    16

/* Seek constants (also in fcntl.h, but stdio needs them too) */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Standard file descriptors */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* FILE structure — Phase 1: no buffering, direct syscall I/O */
typedef struct _IO_FILE {
    int fd;           /* Underlying file descriptor */
    int flags;        /* Flags */
    int err;          /* Error flag */
    int eof;          /* EOF flag */
} FILE;

/* FILE flags */
#define _IO_READ   0x01
#define _IO_WRITE  0x02
#define _IO_APPEND 0x04
#define _IO_ERR    0x08
#define _IO_EOF    0x10

/* Standard streams */
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* File operations */
FILE *fopen(const char *pathname, const char *mode);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *pathname, const char *mode, FILE *stream);
int fclose(FILE *stream);

/* Read / Write */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

/* Character I/O */
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int fputs(const char *s, FILE *stream);
int ungetc(int c, FILE *stream);

/* Formatted output */
int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int sprintf(char *str, const char *fmt, ...);
int snprintf(char *str, size_t size, const char *fmt, ...);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

/* Simple output */
int puts(const char *s);
int putchar(int c);

/* File positioning */
long ftell(FILE *stream);
int fseek(FILE *stream, long offset, int whence);
void rewind(FILE *stream);

/* Status */
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fflush(FILE *stream);

/* Error reporting */
void perror(const char *s);

#endif /* _STDIO_H */
