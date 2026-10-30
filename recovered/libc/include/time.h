#ifndef _TIME_H
#define _TIME_H

#include <stdint.h>
#include <sys/types.h>

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

typedef int64_t time_t;
typedef int64_t suseconds_t;

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};

/* Syscall-backed functions */
time_t time(time_t *tloc);
int clock_gettime(int clk_id, struct timespec *tp);
int gettimeofday(struct timeval *tv, void *tz);
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif /* _TIME_H */
