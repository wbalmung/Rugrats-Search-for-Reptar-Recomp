/* Inline floor / ceil / lround for the per-vertex hot paths (gpu.c, gte.c).

   The build targets baseline x86-64 (SSE2), where clang cannot inline floor/ceil (roundsd is SSE4.1) and never
   inlines lround: every call went through ucrtbase.dll. In the space station (many polygons) those calls were ~13%
   of the main thread (2026-11-12). These versions return exactly the same integers for |x| < 2^52 (all pixel,
   sub-pixel and colour values here); NaN / huge inputs were already undefined when cast to int.

   Include after <math.h>; the macros replace the library calls in that file only. */
#pragma once
#include <math.h>

static inline double rt_floor(double x) {
    double t = (double)(long long)x;   /* truncate toward zero */
    return t > x ? t - 1.0 : t;
}
static inline double rt_ceil(double x) {
    double t = (double)(long long)x;
    return t < x ? t + 1.0 : t;
}
static inline long rt_lround(double x) {   /* half away from zero, like lround */
    long long t = (long long)x;
    double d = x - (double)t;              /* exact for |x| < 2^52 */
    if (d >= 0.5) t++;
    else if (d <= -0.5) t--;
    return (long)t;
}

#define floor(x) rt_floor(x)
#define ceil(x) rt_ceil(x)
#define lround(x) rt_lround(x)
