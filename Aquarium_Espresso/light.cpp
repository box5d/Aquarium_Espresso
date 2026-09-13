#pragma GCC optimize("O3")
#include "light.h"
#include "gfx.h"
#include "bubbles.h"
#include "fastmath.h"
#include <math.h>

// --- tunables ---------------------------------------------------------------
static const float FLICK_HZ      = 8.3f;    // visible beat of the supply ripple
static const float FLICK_DEPTH   = 0.022f;  // ripple, downward only
static const float DIP_DEPTH     = 0.09f;   // deeper stumble of the driver
static const int   CAUSTIC_AMP   = 52;      // shadow depth between bands, in water

// The caustic net is a thing you see *on* something. In open water it is a
// faint shimmer; where the light finally lands - the sand - it resolves into
// the hard, moving lace that is the whole look of a lit tank. So the substrate
// gets its own, much stronger amplitude, and its own geometry: the pattern is
// a projection onto a horizontal plane running away from the viewer, which
// means it compresses towards the far edge and spreads out at the near one.
static const int   FLOOR_AMP   = 116;       // how much harder the net bites on sand
static const float FLOOR_TOP   = 150.0f;    // rows: where the substrate starts
static const float FLOOR_BOT   = 240.0f;    // ... and the bottom of the screen
static const float FLOOR_FADE  = 24.0f;     // rows the net takes to come up
static const float FLOOR_PERSP = 0.45f;     // smaller = steeper perspective
static const float FLOOR_NEAR  = 0.75f;     // net scale at the near edge
static const int   POOL_AMP      = 30;      // depth of the broad drifting pool

// What the air pump does to the light.
//
// The caustic net is a picture of the water surface: a still surface throws a
// slow, lazy, soft net, and a surface being punched forty times a second by
// bursting bubbles throws a fast, hard, restless one. bubbles.cpp measures how
// hard the pump is working the surface; these say what that is worth.
//
// All of it is per-frame or per-row, so the agitation costs nothing per pixel.
static const float AGIT_SPEED = 1.45f;      // how much faster the net travels
static const float AGIT_SWAY  = 0.90f;      // how much faster it breathes
static const float AGIT_MORPH = 1.70f;      // how much faster the forms re-make
static const int   AGIT_BITE  = 78;         // extra net contrast, 8.8 fixed

// Backdrop exposure, applied once at boot.
//
//   y   = in^GAMMA * GAIN
//   out = y * (1 + y/GAIN^2) / (1 + y)          (extended Reinhard shoulder)
//
// This is an exposure push, not a shadow lift. GAMMA above 1 pushes the deep
// water further down while GAIN opens up everything above it, and the shoulder
// rolls the highlights into white instead of clipping them - out(1) is exactly
// 1 by construction. Lifting the black point instead (which is the obvious
// thing to try) just makes the whole tank look hazy.
//
// Over the five backdrops: 0.02 -> 0.02, 0.05 -> 0.06, 0.15 -> 0.23,
// 0.30 -> 0.45, 0.50 -> 0.66, 0.70 -> 0.82, 0.90 -> 0.94 - no clipping at all.
// Raise GAIN for more exposure, GAMMA for deeper shadows.
static const float EXPO_GAMMA = 1.35f;
static const float EXPO_GAIN  = 3.8f;

static const int LUT_N = 512;
static uint8_t causticLUT[LUT_N];           // sharpened wave profile, mean 128
static uint8_t poolLUT[LUT_N];              // smooth, for the broad wave

// The three sharp trains, as a direction and a wavenumber rather than as a
// vector, because both of those are made to drift: see morphT below. Their
// directions are roughly 60 degrees apart, which is what turns a plaid into
// cells. Wavelengths are all near 110px, a little over a third of the tank.
//
// Wave C is not one of them - its wavelength is longer than the tank is wide,
// so it reads as the lit area itself sliding about rather than as a band.
static const float A_ANG = 0.36f,  A_K = 0.0589f;
static const float B_ANG = -0.945f, B_K = 0.0580f;
static const float D_ANG = 1.396f, D_K = 0.0533f;
static const float KCX = 0.013f, KCY = 0.009f;

static float phaseA = 0, phaseB = 0, phaseC = 0, phaseD = 0;
static float morphT = 0;
static float swayT = 0, flickPhase = 0;
static float dipTimer = 3.0f, dipT = 0, dipLen = 0.1f;
static float gain = 1.0f;                   // current global brightness
static int   ampScale = 256;                // caustic contrast, 8.8 fixed

static uint8_t rowAmp[SCR_H];
// Each sharp wave gets its phase and its horizontal rate per row rather than
// from a constant, so the substrate can carry a perspective-projected version
// of the same net the water carries - and so the directions can drift without
// costing anything in the pixel loop.
static int32_t rowPhA[SCR_H], rowPhB[SCR_H], rowPhD[SCR_H];
static int32_t rowStepA[SCR_H], rowStepB[SCR_H], rowStepD[SCR_H];
static float   floorMix[SCR_H];   // 0 in open water, 1 on the sand

static inline float frand() { return (float)esp_random() / 4294967296.0f; }

void lightInit() {
  for (int i = 0; i < LUT_N; i++) {
    float u = i * (float)(M_PI * 2 / LUT_N);
    float v = sinf(u) * 0.60f + sinf(u * 2.3f + 1.1f) * 0.30f
            + sinf(u * 3.7f + 2.4f) * 0.18f;
    // sharpen: caustics are thin bright lines over a broad soft shadow
    float s = (v > 0) ? powf(v, 2.2f) : -powf(-v, 1.6f) * 0.35f;
    int q = 128 + (int)(s * 127.0f);
    causticLUT[i] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
    poolLUT[i] = (uint8_t)(128 + (int)(sinf(u) * 126.0f));
  }
  // Light comes from the bar above the tank: through the water it is a soft
  // shimmer that washes out with depth, and on the sand it lands as the net.
  for (int y = 0; y < SCR_H; y++) {
    float d = (float)y / (SCR_H - 1);
    float water = CAUSTIC_AMP * (1.0f - 0.55f * d * d);

    // The sand mask comes up over a couple of dozen rows so the net arrives
    // where the substrate does, and is then flat across the rest of the band -
    // the perspective below needs the whole band, not a ramp.
    float t = ((float)y - FLOOR_TOP) / FLOOR_FADE;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    float m = t * t * (3.0f - 2.0f * t);
    floorMix[y] = m;

    // Three trains are summed where there used to be two, so each one gets
    // two thirds of the budget and the contrast at the peaks is unchanged.
    // (It also keeps the gain non-negative without a clamp in the pixel loop.)
    float a = (water * (1.0f - m) + FLOOR_AMP * m) * (2.0f / 3.0f);
    rowAmp[y] = (uint8_t)(a > 255 ? 255 : a);
  }
}

void lightStep(float dt) {
  // The surface is never still, so the whole net speeds up and slows down -
  // and with the air pump running it is never anywhere near still, so all of
  // that is scaled by how hard the bubbles are breaking through.
  const float ag = airAgitation();
  const float spd = 1.0f + AGIT_SPEED * ag;
  ampScale = 256 + (int)(AGIT_BITE * ag);

  swayT += dt * 0.21f * (1.0f + AGIT_SWAY * ag);
  float sw = sinf(swayT);
  phaseA   += dt * (0.75f + 0.55f * sw) * spd;
  phaseB   -= dt * (0.45f + 0.35f * sinf(swayT * 0.7f + 1.3f)) * spd;
  phaseD   += dt * (0.58f + 0.30f * sinf(swayT * 1.3f + 2.6f)) * spd;
  // the broad pool is the lamp sliding about, not the surface, so it is only
  // lightly affected
  phaseC   += dt * (0.16f + 0.13f * sinf(swayT * 0.5f - 0.6f)) * (1.0f + 0.35f * ag);
  // ... and this is the one that makes the net re-form rather than scroll
  morphT   += dt * (0.115f + AGIT_MORPH * 0.115f * ag);

  // These get scaled by ~5.3 million on the way into a 16.16 accumulator, so
  // an unwrapped phase overflows int32 in about nine minutes and the net stops
  // moving. The profile LUT is 2pi periodic, so wrapping costs nothing.
  const float TAU = (float)(M_PI * 2);
  while (phaseA >= TAU) phaseA -= TAU;
  while (phaseA < 0)    phaseA += TAU;
  while (phaseB >= TAU) phaseB -= TAU;
  while (phaseB < 0)    phaseB += TAU;
  while (phaseC >= TAU) phaseC -= TAU;
  while (phaseC < 0)    phaseC += TAU;
  while (phaseD >= TAU) phaseD -= TAU;
  while (phaseD < 0)    phaseD += TAU;
  while (swayT  >= TAU * 64) swayT -= TAU * 64;
  while (morphT >= TAU * 64) morphT -= TAU * 64;

  flickPhase += dt * (float)(M_PI * 2) * FLICK_HZ;
  if (flickPhase > (float)(M_PI * 2) * 1024) flickPhase -= (float)(M_PI * 2) * 1024;
  float rip = 0.6f * sinf(flickPhase) + 0.4f * sinf(flickPhase * 2 + 0.7f);
  // ripple downward only: the caustic peaks are then exactly full brightness,
  // which lets the pixel loop drop its per-channel clamp
  float g = 1.0f - FLICK_DEPTH * (1.0f - rip);

  dipTimer -= dt;
  if (dipTimer <= 0 && dipT <= 0) {
    dipTimer = 2.5f + frand() * 7.0f;
    dipLen = 0.05f + frand() * 0.12f;
    dipT = dipLen;
  }
  if (dipT > 0) {
    dipT -= dt;
    float p = 1.0f - (dipT > 0 ? dipT / dipLen : 0.0f);
    g *= 1.0f - DIP_DEPTH * sinf((float)M_PI * p);
  }
  gain = g;

  // Where the three trains are pointing this frame.
  //
  // Caustics are not a pattern that slides past; they are a pattern that keeps
  // re-making itself in place, because the surface lenses throwing it are
  // themselves being re-shaped. Drifting each train's direction and wavelength
  // does exactly that - the lines wander, cross each other differently, cells
  // open and close - and because it is resolved once a frame it is free.
  const float aA = A_ANG + 0.34f * fsin(morphT * 0.83f);
  const float aB = B_ANG + 0.29f * fsin(morphT * 0.61f + 2.1f);
  const float aD = D_ANG + 0.31f * fsin(morphT * 1.07f + 4.4f);
  const float kA = A_K * (1.0f + 0.17f * fsin(morphT * 0.47f + 1.7f));
  const float kB = B_K * (1.0f + 0.15f * fsin(morphT * 0.71f - 0.9f));
  const float kD = D_K * (1.0f + 0.19f * fsin(morphT * 0.55f + 3.3f));
  const float kax = kA * fcos(aA), kay = kA * fsin(aA);
  const float kbx = kB * fcos(aB), kby = kB * fsin(aB);
  const float kdx = kD * fcos(aD), kdy = kD * fsin(aD);

  // Per-row phase and horizontal rate.
  //
  // In open water the net lives in the vertical plane the screen is: constant
  // rate along a row, phase stepping evenly down the screen. On the sand it is
  // the same net projected onto a floor running away from the viewer, so a
  // fixed spacing out in the tank covers fewer screen pixels the further back
  // it is - the lace packs together towards the far edge of the substrate and
  // opens out at the near one. That single cue is most of what says "this is
  // lying on the bottom" rather than "this is painted on the glass".
  //
  // Doing it per row costs nothing: the pixel loop already reads a step and a
  // phase, it just reads them from a table now.
  const float toIdx = LUT_N / (float)(M_PI * 2);
  const float baseA = kax * toIdx * 65536.0f;
  const float baseB = kbx * toIdx * 65536.0f;
  const float baseD = kdx * toIdx * 65536.0f;
  for (int y = 0; y < SCR_H; y++) {
    float m = floorMix[y];
    float persp = 1.0f;
    if (m > 0.0f) {
      float u = ((float)y - FLOOR_TOP) / (FLOOR_BOT - FLOOR_TOP);
      u = u < 0 ? 0 : (u > 1 ? 1 : u);
      // depth along the floor: far edge at the top of the band, near at the
      // bottom of the screen
      float z = 1.0f / (FLOOR_PERSP + u * (1.0f - FLOOR_PERSP));
      persp = 1.0f + m * (FLOOR_NEAR * z - 1.0f);
    }
    rowPhA[y] = (int32_t)((kay * y + phaseA) * toIdx * 65536.0f);
    rowPhB[y] = (int32_t)((kby * y + phaseB) * toIdx * 65536.0f);
    rowPhD[y] = (int32_t)((kdy * y + phaseD) * toIdx * 65536.0f);
    rowStepA[y] = (int32_t)(baseA * persp);
    rowStepB[y] = (int32_t)(baseB * persp);
    rowStepD[y] = (int32_t)(baseD * persp);
  }
}

// 8.8 fixed point, 256 = unity. The gain never exceeds 256, so no clamping is
// needed: the caustic term only ever darkens, with the bright bands sitting at
// the backdrop's own brightness.
static inline uint16_t litPixel(uint16_t c, int g) {
  int r  = ((c >> 11) * g) >> 8;
  int gg = (((c >> 5) & 0x3F) * g) >> 8;
  int b  = ((c & 0x1F) * g) >> 8;
  return (uint16_t)((r << 11) | (gg << 5) | b);
}

// One gain value per pixel pair: the caustic wavelength is ~100px, so the pair
// granularity is invisible and it halves the wave-LUT traffic.
static inline int nextGain(int gTop, int amp, int pool,
                           int32_t& ua, int32_t& ub, int32_t& ud,
                           int32_t dA, int32_t dB, int32_t dD) {
  int wa = 255 - causticLUT[(ua >> 16) & (LUT_N - 1)];
  int wb = 255 - causticLUT[(ub >> 16) & (LUT_N - 1)];
  int wd = 255 - causticLUT[(ud >> 16) & (LUT_N - 1)];
  ua += dA + dA;
  ub += dB + dB;
  ud += dD + dD;
  return gTop - (((wa + wb + wd) * amp) >> 9) - pool;
}

// The broad wave is longer than the tank is wide, so it barely moves across
// eight pixels. Resolving it once per eight and reading it back as a byte
// keeps it nearly free - sampling it in the pixel loop cost as much as the two
// sharp waves put together.
static uint8_t poolRow2[2][SCR_W / 8 + 2];

static void IRAM_ATTR buildPoolRow(uint8_t* poolRow, int32_t uc, int32_t dC) {
  const int32_t step = dC * 8;
  for (int i = 0; i < SCR_W / 8 + 2; i++) {
    poolRow[i] = (uint8_t)(((255 - poolLUT[(uc >> 16) & (LUT_N - 1)]) * POOL_AMP) >> 9);
    uc += step;
  }
}

void IRAM_ATTR lightApply(int y0, int y1) {
  uint8_t* const poolRow = poolRow2[xPortGetCoreID()];
  const int gTop = (int)(gain * 256.0f + 0.5f);
  // 16.16 accumulators stepping the wave LUTs across the row
  const int32_t dC = (int32_t)(KCX * LUT_N / (M_PI * 2) * 65536.0f);
  const float toIdx = LUT_N / (float)(M_PI * 2);

  for (int y = y0; y < y1; y++) {
    const int amp = (rowAmp[y] * ampScale) >> 8;
    const int32_t dA = rowStepA[y], dB = rowStepB[y], dD = rowStepD[y];
    int32_t ua = rowPhA[y];
    int32_t ub = rowPhB[y];
    int32_t ud = rowPhD[y];
    int32_t uc = (int32_t)((KCY * y + phaseC) * toIdx * 65536.0f);

    const uint16_t* src = BGBUF + (size_t)y * SCR_W;
    uint16_t*       dst = FB    + (size_t)y * SCR_W;

    // Straight across. The scene is a photograph of a tank and it stays where
    // it is: the light moves over it, it does not move under the light.
    buildPoolRow(poolRow, uc, dC);
    for (int x = 0; x < SCR_W; x += 2) {
      const int g = nextGain(gTop, amp, poolRow[x >> 3], ua, ub, ud, dA, dB, dD);
      dst[x]     = litPixel(src[x], g);
      dst[x + 1] = litPixel(src[x + 1], g);
    }
  }
}

int lightGainAt(int x, int y) {
  if (y < 0) y = 0; else if (y >= SCR_H) y = SCR_H - 1;
  if (x < 0) x = 0; else if (x >= SCR_W) x = SCR_W - 1;
  const float toIdx = LUT_N / (float)(M_PI * 2);
  int ia = (int)((rowPhA[y] + (int64_t)rowStepA[y] * x) >> 16) & (LUT_N - 1);
  int ib = (int)((rowPhB[y] + (int64_t)rowStepB[y] * x) >> 16) & (LUT_N - 1);
  int id = (int)((rowPhD[y] + (int64_t)rowStepD[y] * x) >> 16) & (LUT_N - 1);
  int ic = (int)((KCX * x + KCY * y + phaseC) * toIdx) & (LUT_N - 1);
  int wa = 255 - causticLUT[ia];
  int wb = 255 - causticLUT[ib];
  int wd = 255 - causticLUT[id];
  int wc = 255 - poolLUT[ic];
  int g = (int)(gain * 256.0f + 0.5f)
        - (((wa + wb + wd) * ((rowAmp[y] * ampScale) >> 8)) >> 9)
        - ((wc * POOL_AMP) >> 9);
  return g < 32 ? 32 : (g > 256 ? 256 : g);
}

static inline float expose(float v) {
  float y = powf(v, EXPO_GAMMA) * EXPO_GAIN;
  return y * (1.0f + y / (EXPO_GAIN * EXPO_GAIN)) / (1.0f + y);
}

void lightPrepBackdrop() {
  // one tone curve per bit depth, so the whole backdrop is three table lookups
  // a pixel rather than any arithmetic
  uint8_t c5[32], c6[64];
  for (int i = 0; i < 32; i++) {
    int q = (int)(expose(i / 31.0f) * 31.0f + 0.5f);
    c5[i] = (uint8_t)(q < 0 ? 0 : (q > 31 ? 31 : q));
  }
  for (int i = 0; i < 64; i++) {
    int q = (int)(expose(i / 63.0f) * 63.0f + 0.5f);
    c6[i] = (uint8_t)(q < 0 ? 0 : (q > 63 ? 63 : q));
  }
  uint16_t* p = BGBUF;
  for (size_t i = 0; i < (size_t)SCR_W * SCR_H; i++) {
    uint16_t c = p[i];
    p[i] = (uint16_t)((c5[c >> 11] << 11) | (c6[(c >> 5) & 0x3F] << 5) | c5[c & 0x1F]);
  }
}
