/* SpiritFoxOS libc - POSIX unistd convenience wrappers
 *
 * Only contains functions NOT already implemented in syscall.c.
 * read/write/close/fork/execve/etc are in syscall.c.
 */

#include <unistd.h>
#include <time.h>
#include "internal.h"

unsigned int sleep(unsigned int seconds)
{
    struct timespec req = { .tv_sec = seconds, .tv_nsec = 0 };
    struct timespec rem;
    if (nanosleep(&req, &rem) < 0)
        return (unsigned int)rem.tv_sec;
    return 0;
}

int usleep(useconds_t usec)
{
    struct timespec req = {
        .tv_sec = usec / 1000000,
        .tv_nsec = (long)(usec % 1000000) * 1000
    };
    return nanosleep(&req, NULL);
}
