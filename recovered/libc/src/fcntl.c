/* SpiritFoxOS libc - fcntl extensions */

#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include "internal.h"

int creat(const char *path, mode_t mode)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    /* Fallback: kernel doesn't support openat yet, use open */
    (void)dirfd;
    return open(path, flags, mode);
}
