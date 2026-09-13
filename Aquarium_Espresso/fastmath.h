#ifndef PIXAQ_FASTMATH_H
#define PIXAQ_FASTMATH_H
// ---------------------------------------------------------------------------
// fastmath.h — 1024-entry sine table with linear interpolation.
//
// newlib's sinf spends most of its time on argument reduction, and it gets
// slower the larger the argument is. The wave phases here run into the
// hundreds of radians, and the tail undulation alone evaluates a sine per
// sprite row per segment per fish - profiling showed that costing more than
// the pixel loop it feeds, and drifting worse as the phases grew.
//
// These waves are decorative, so a table is both accurate enough (~1e-5) and
// roughly an order of magnitude faster, with a cost that does not depend on
// how far the phase has run.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <math.h>

extern float SIN_LUT[1025];
void fastMathInit();

static inline float fsin(float x) {
  float t = x * (float)(1024.0 / (2 * M_PI));
  int i = (int)t;
  if (t < 0) i--;
  float fr = t - (float)i;
  i &= 1023;
  return SIN_LUT[i] + (SIN_LUT[i + 1] - SIN_LUT[i]) * fr;
}

static inline float fcos(float x) { return fsin(x + (float)M_PI_2); }

// cheap angle wrap; the inputs here are always within a turn or two of range
static inline float fwrap(float a) {
  while (a > (float)M_PI)  a -= (float)(2 * M_PI);
  while (a < -(float)M_PI) a += (float)(2 * M_PI);
  return a;
}

#endif // PIXAQ_FASTMATH_H
