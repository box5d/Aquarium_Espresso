#pragma GCC optimize("O3")
#include "bubbles.h"
#include "gfx.h"
#include "light.h"
#include "fastmath.h"
#include <math.h>

// --- geometry ---------------------------------------------------------------
// The backdrops all frame the same tank, so the water line and the sand sit at
// the same rows in every one of them. These match VIEW::horizonY and the sand
// band light.cpp shades.
static const float SURF_Y   = 16.0f;    // where a bubble breaks
static const float STONE_Y0 = 206.0f;   // the stone is buried in the sand
static const float STONE_Y1 = 219.0f;
// ... towards one end or the other, never in the middle where the fish are
static const float L_X0 = 20.0f,  L_X1 = 74.0f;
static const float R_X0 = 246.0f, R_X1 = 300.0f;

// --- the pump ---------------------------------------------------------------
// A diaphragm pump is a rubber membrane flapping behind a small buffer, so what
// leaves the stone is not a constant stream but a rate that swells and thins a
// couple of times a second.
static const float PUFF_HZ    = 26.0f;  // puffs per second
static const float THROB_HZ   = 2.3f;   // the pump's own pulse
static const float THROB_AMT  = 0.42f;

// A fine-pore stone makes a mist of bubbles, not a chain of beads: sub-
// millimetre, which at this scale is well under a pixel across, and slow -
// rise speed falls off fast as they get smaller. There have to be a lot of
// them, so the cap is what the plume can hold at that rate.
static const float R_MIN = 0.34f, R_MAX = 0.78f;

static const int MAX_BUB = 232;
static const int MAX_POP = 32;

struct Stone {
  float x, y;
  float emit;      // puffs owed, counted down
  float throb;     // pump phase
  float lean;      // the column drifts as it climbs, on the tank's own current
  float leanT;
};

// How wide the plume is allowed to get by the time it reaches the surface. A
// stone releases its bubbles within a centimetre or two, and they leave more or
// less straight up; what opens the plume out on the way is that each one takes
// its own path through the water it has already stirred up. So the fan is not a
// property of the stone, it is a property of the climb - which is why every
// lateral term here is scaled by how far up the bubble has got.
static const float FAN_PX = 26.0f;

// --- the water the plume drags with it --------------------------------------
// Peak speed of the chimney, px/s, for a fish small enough to go along with it
// completely. Measured against the fish: a neon tetra cruises at 14px/s, so a
// core this strong is unmistakably something happening *to* the fish rather
// than something it is doing.
static const float LIFT_MAX    = 62.0f;
static const float LIFT_SPREAD = 34.0f;  // outward flow where it hits the top
static const float LIFT_CORE   = 5.0f;   // half-width at the stone

static inline float smoothstep(float a, float b, float v) {
  float t = (v - a) / (b - a);
  if (t <= 0) return 0.0f;
  if (t >= 1) return 1.0f;
  return t * t * (3.0f - 2.0f * t);
}

struct Bubble {
  float x0;        // column centre at the stone
  float y;
  float r0, r;     // radius at the stone / right now
  float vy;
  float fan;       // this one's share of the plume's spread, -1 .. 1
  float wobA, wobK, wobP;   // the spiral a rising bubble traces
  uint8_t live;
};

// A burst ring on the surface, flattened hard because the water line is seen
// almost edge-on.
struct Pop {
  float x, t, dur, r1;
  uint8_t live;
};

static Stone  stone;
static Bubble bub[MAX_BUB];
static Pop    pop[MAX_POP];
static int    nextBub = 0, nextPop = 0;
static float  agit = 0.55f;

static inline float frand() { return (float)esp_random() / 4294967296.0f; }
static inline float rrange(float a, float b) { return a + frand() * (b - a); }

void bubblesInit() {
  stone.x     = (frand() < 0.5f) ? rrange(L_X0, L_X1) : rrange(R_X0, R_X1);
  stone.y     = rrange(STONE_Y0, STONE_Y1);
  stone.emit  = frand();
  stone.throb = frand() * 6.283f;
  stone.lean  = 0;
  stone.leanT = frand() * 6.283f;
  for (int i = 0; i < MAX_BUB; i++) bub[i].live = 0;
  for (int i = 0; i < MAX_POP; i++) pop[i].live = 0;
  Serial.printf("air stone at x=%.0f\n", stone.x);
}

static void spawn() {
  Bubble& b = bub[nextBub];
  nextBub = (nextBub + 1 == MAX_BUB) ? 0 : nextBub + 1;

  // Everything leaves the stone within a couple of pixels of the same place.
  b.x0 = stone.x + rrange(-1.6f, 1.6f);
  b.y  = stone.y;
  b.r0 = rrange(R_MIN, R_MAX);
  b.r  = b.r0;
  // Bigger ones rise faster: drag goes as the area, buoyancy as the volume.
  b.vy = 50.0f + b.r0 * 44.0f + rrange(-5.0f, 5.0f);
  // Which side of the plume this one ends up on, biased towards the middle so
  // the fan has a dense core and thin edges rather than a hard rim.
  float t = frand() + frand() - 1.0f;
  b.fan = t * t * t * 0.5f + t * 0.5f;
  // the zigzag - small bubbles wander further per unit of rise than large ones
  b.wobA = rrange(0.7f, 2.0f) / (0.4f + b.r0);
  b.wobK = rrange(0.070f, 0.150f);
  b.wobP = frand() * 6.283f;
  b.live = 1;
}

static void burst(float x) {
  Pop& p = pop[nextPop];
  nextPop = (nextPop + 1 == MAX_POP) ? 0 : nextPop + 1;
  p.x   = x;
  p.t   = 0;
  p.dur = rrange(0.22f, 0.40f);
  p.r1  = rrange(3.5f, 7.0f);
  p.live = 1;
}

void bubblesStep(float dt) {
  // --- the stone emits -------------------------------------------------------
  {
    Stone& st = stone;
    st.throb += dt * (float)(2 * M_PI) * THROB_HZ;
    if (st.throb > (float)(2 * M_PI) * 64) st.throb -= (float)(2 * M_PI) * 64;
    st.leanT += dt * 0.37f;
    if (st.leanT > (float)(2 * M_PI) * 64) st.leanT -= (float)(2 * M_PI) * 64;
    st.lean = fsin(st.leanT) * 3.4f + fsin(st.leanT * 2.3f + 1.1f) * 1.6f;

    float rate = PUFF_HZ * (1.0f + THROB_AMT * fsin(st.throb));
    st.emit -= dt * rate;
    while (st.emit <= 0) {
      st.emit += 1.0f;
      spawn();
      if (frand() < 0.70f) spawn();        // a fine stone releases in clouds
      if (frand() < 0.40f) spawn();
      if (frand() < 0.15f) spawn();
    }
  }

  // --- bubbles rise ----------------------------------------------------------
  const float span = 1.0f / (STONE_Y0 - SURF_Y);
  for (int i = 0; i < MAX_BUB; i++) {
    Bubble& b = bub[i];
    if (!b.live) continue;
    // The climb accelerates: the water column above gets shorter, the bubble
    // expands against the falling pressure, buoyancy wins.
    float u = (stone.y - b.y) * span;            // 0 at the stone, 1 at the top
    if (u < 0) u = 0; else if (u > 1) u = 1;
    b.r = b.r0 * (1.0f + 0.42f * u);
    b.y -= b.vy * (1.0f + 0.22f * u) * dt;
    if (b.y <= SURF_Y) {
      b.live = 0;
      burst(b.x0 + b.fan * FAN_PX + stone.lean);
      // Every burst is a dent in the surface, and the surface is what throws
      // the caustic net. This is the only thing that drives `agit`.
      agit += 0.026f;
    }
  }

  // --- surface rings ---------------------------------------------------------
  for (int i = 0; i < MAX_POP; i++) {
    if (!pop[i].live) continue;
    pop[i].t += dt;
    if (pop[i].t >= pop[i].dur) pop[i].live = 0;
  }

  // The surface would settle in about half a second if the air stopped, so at
  // forty-odd bursts a second it never settles - it breathes with the pump.
  agit -= agit * dt * 2.15f;
  if (agit < 0) agit = 0; else if (agit > 1.0f) agit = 1.0f;
}

float airAgitation() { return agit; }

void airFlowAt(float x, float y, float* vx, float* vy) {
  *vx = 0.0f;
  *vy = 0.0f;

  const float u = (stone.y - y) * (1.0f / (STONE_Y0 - SURF_Y));
  if (u <= 0.0f || u >= 1.06f) return;

  // Same cone the bubbles climb, a little wider: the sleeve of water each
  // bubble drags is bigger than the bubble.
  const float w = LIFT_CORE + FAN_PX * u * u;
  const float dx = x - (stone.x + stone.lean * u * u);
  const float t = dx / w;
  if (t <= -1.0f || t >= 1.0f) return;
  const float rad = 1.0f - t * t;            // soft-edged core

  // The column has to be built before it can carry anything: right at the
  // stone the water has not been accelerated yet, and by the time it reaches
  // the top it is already turning over and running away sideways.
  const float env = smoothstep(0.0f, 0.22f, u)
                  * (1.0f - 0.70f * smoothstep(0.78f, 1.04f, u));

  *vy = -LIFT_MAX * env * rad;
  // ... and that turning-over is what finally throws a fish clear, instead of
  // parking it against the surface
  *vx = LIFT_SPREAD * smoothstep(0.72f, 1.04f, u) * t * rad;
}

// ---------------------------------------------------------------------------
// Drawing. A bubble off a fine stone is smaller than a pixel for most of its
// climb, so there is no shading to do: what lands on the panel is a point of
// light, brightest where the rim of the lens throws back the lamp, softening to
// nothing over the pixel it sits in. The arithmetic below is the same rim model
// a bigger bubble would get - it just never gets wider than a pixel or two.
// ---------------------------------------------------------------------------
static inline void drawBubble(float cx, float cy, float r, uint16_t col,
                              uint16_t hi, int y0, int y1) {
  int ix0 = (int)floorf(cx - r - 1.0f), ix1 = (int)ceilf(cx + r + 1.0f);
  int iy0 = (int)floorf(cy - r - 1.0f), iy1 = (int)ceilf(cy + r + 1.0f);
  if (ix0 < 0) ix0 = 0;
  if (ix1 > SCR_W) ix1 = SCR_W;
  if (iy0 < y0) iy0 = y0;
  if (iy1 > y1) iy1 = y1;

  const float rim = r - 0.28f;
  for (int y = iy0; y < iy1; y++) {
    float dy = (float)y + 0.5f - cy;
    for (int x = ix0; x < ix1; x++) {
      float dx = (float)x + 0.5f - cx;
      float d = sqrtf(dx * dx + dy * dy);
      float cov = r + 0.5f - d;                 // disc coverage
      if (cov <= 0) continue;
      if (cov > 1) cov = 1;
      float e = 1.0f - fabsf(d - rim) * 1.15f;  // how close to the rim
      if (e < 0) e = 0;
      int al = (int)(cov * (0.14f + 0.72f * e) * 32.0f + 0.5f);
      if (al > 0) px_blend(x, y, col, al);
    }
  }
  // the pip the lamp above leaves on the shoulder of the bubble
  if (r >= 0.95f) {
    int hx = (int)(cx - r * 0.34f + 0.5f);
    int hy = (int)(cy - r * 0.40f + 0.5f);
    if (hy >= y0 && hy < y1) px_blend(hx, hy, hi, 19);
  }
}

static inline void drawPop(const Pop& p, uint16_t col, int y0, int y1) {
  float u  = p.t / p.dur;
  float rx = 1.2f + (p.r1 - 1.2f) * u;
  float ry = rx * 0.30f + 0.6f;
  float a  = (1.0f - u) * (1.0f - u) * 0.60f;
  if (a <= 0.01f) return;

  int ix0 = (int)floorf(p.x - rx - 1),      ix1 = (int)ceilf(p.x + rx + 1);
  int iy0 = (int)floorf(SURF_Y - ry - 1),   iy1 = (int)ceilf(SURF_Y + ry + 1);
  if (ix0 < 0) ix0 = 0;
  if (ix1 > SCR_W) ix1 = SCR_W;
  if (iy0 < y0) iy0 = y0;
  if (iy1 > y1) iy1 = y1;

  const float irx = 1.0f / rx, iry = 1.0f / ry;
  for (int y = iy0; y < iy1; y++) {
    float dy = ((float)y + 0.5f - SURF_Y) * iry;
    for (int x = ix0; x < ix1; x++) {
      float dx = ((float)x + 0.5f - p.x) * irx;
      float e = sqrtf(dx * dx + dy * dy);
      float w = 1.0f - fabsf(e - 1.0f) * 3.2f;
      if (w <= 0) continue;
      int al = (int)(a * w * 32.0f + 0.5f);
      if (al > 0) px_blend(x, y, col, al);
    }
  }
}

void bubblesDraw(int y0, int y1) {
  // Bubbles are glass: they take the tank's light like everything else, or they
  // sit on top of the picture the way the fish used to.
  for (int i = 0; i < MAX_BUB; i++) {
    const Bubble& b = bub[i];
    if (!b.live) continue;
    const float r = b.r;
    if (b.y + r + 1 < y0 || b.y - r - 1 >= y1) continue;

    float u = (stone.y - b.y) * (1.0f / (STONE_Y0 - SURF_Y));
    if (u < 0) u = 0; else if (u > 1) u = 1;
    // The fan and the spiral both open out with the climb, and the whole column
    // leans on the tank's current. u*u rather than u: the plume leaves the sand
    // as a narrow stalk and only spreads properly in the upper half.
    float x = b.x0 + b.fan * FAN_PX * u * u
            + fsin(b.y * b.wobK + b.wobP) * b.wobA * (0.25f + 0.75f * u)
            + stone.lean * u * u;

    int g = lightGainAt((int)x, (int)b.y);
    uint16_t col = rgb565((202 * g) >> 8, (236 * g) >> 8, (255 * g) >> 8);
    uint16_t hi  = rgb565((250 * g) >> 8, (255 * g) >> 8, (255 * g) >> 8);
    drawBubble(x, b.y, r, col, hi, y0, y1);
  }

  if (SURF_Y + 8 < y0 || SURF_Y - 8 >= y1) return;
  const int g = lightGainAt(SCR_W / 2, (int)SURF_Y);
  const uint16_t col = rgb565((226 * g) >> 8, (246 * g) >> 8, (255 * g) >> 8);
  for (int i = 0; i < MAX_POP; i++)
    if (pop[i].live) drawPop(pop[i], col, y0, y1);
}
