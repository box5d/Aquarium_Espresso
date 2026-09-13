#include "fastmath.h"

float SIN_LUT[1025];

void fastMathInit() {
  for (int i = 0; i <= 1024; i++) SIN_LUT[i] = sinf(i * (float)(2 * M_PI / 1024));
}
