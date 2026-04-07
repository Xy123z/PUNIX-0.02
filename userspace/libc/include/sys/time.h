#ifndef SYS_TIME_H
#define SYS_TIME_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct timeval {
    time_t tv_sec;
    uint32_t tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval *tv, struct timezone *tz);

#endif
