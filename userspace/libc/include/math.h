#ifndef _MATH_H
#define _MATH_H

#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))

// Basic double versions
double floor(double x);
double ceil(double x);
double fabs(double x);
double log(double x);
double exp(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double sqrt(double x);
double atan2(double y, double x);

// Float versions
float floorf(float x);
float ceilf(float x);
float fabsf(float x);
float logf(float x);
float expf(float x);
float powf(float x, float y);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float sqrtf(float x);
float atan2f(float y, float x);

// Long double versions (essential for TCC internal math)
long double floorl(long double x);
long double ceill(long double x);
long double fabsl(long double x);
long double logl(long double x);
long double expl(long double x);
long double powl(long double x, long double y);
long double sinl(long double x);
long double cosl(long double x);
long double tanl(long double x);
long double sqrtl(long double x);
long double atan2l(long double y, long double x);

#endif
