/* SpiritFoxOS libc - Time functions */

#include <time.h>
#include <unistd.h>
#include "internal.h"

time_t time(time_t *tloc)
{
    int64_t ret = sfk_syscall1(SYS_time, tloc ? (int64_t)tloc : 0);
    if (ret < 0) { sfk_errno = -(int)ret; return -1; }
    if (tloc) *tloc = (time_t)ret;
    return (time_t)ret;
}

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
