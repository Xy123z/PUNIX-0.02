#ifndef ASSERT_H
#define ASSERT_H

#include <stdio.h>
#include <stdlib.h>

#define assert(expr) \
    ((expr) ? (void)0 : (fprintf(stderr, "Assertion failed: %s, file %s, line %d\n", #expr, __FILE__, __LINE__), abort()))

void abort(void);

#endif
