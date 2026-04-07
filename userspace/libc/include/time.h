#ifndef TIME_H
#define TIME_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t time_t;

time_t time(time_t *tloc);

#endif
