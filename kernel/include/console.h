#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdarg.h>

/* Console output functions */
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);

#endif /* CONSOLE_H */
