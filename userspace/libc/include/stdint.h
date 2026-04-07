#ifndef STDINT_H
#define STDINT_H

typedef unsigned int   uint32_t;
typedef int            int32_t;
typedef unsigned short uint16_t;
typedef short          int16_t;
typedef unsigned char  uint8_t;
typedef char           int8_t;
typedef unsigned long long uint64_t;
typedef long long          int64_t;

typedef uint32_t uintptr_t;
typedef int32_t  intptr_t;

#define UINT32_MAX 4294967295U
#define INT32_MAX  2147483647
#define UINT16_MAX 65535

#endif
