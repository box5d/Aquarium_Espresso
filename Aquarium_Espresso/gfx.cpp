#pragma GCC optimize("O3")
#include "gfx.h"

uint16_t* FB    = nullptr;
uint16_t* BGBUF = nullptr;

int gDX0 = 0, gDY0 = 0, gDX1 = 0, gDY1 = 0;
int gClipY0[2] = { 0, 0 }, gClipY1[2] = { FB_H, FB_H };

// ---------------------------------------------------------------------------
// Anti-aliased polygon fill. Three sub-scanlines per pixel row, exact
// horizontal span coverage; that is enough to keep 12px fish and 1px fins from
// crawling, at a fraction of the cost of full supersampling.
// ---------------------------------------------------------------------------
// One scratch row per core: the two halves of a band are rasterised at the
// same time on different cores, and they must not share it.
static float covRow2[2][FB_W];

void IRAM_ATTR fillPolyAA(const float* xs, const float* ys, int n, const PolyPaint& p) {
  if (n < 3) return;
  const int cid = xPortGetCoreID();
  float* const covRow = covRow2[cid];
  float ymin = ys[0], ymax = ys[0], xmin = xs[0], xmax = xs[0];
  for (int i = 1; i < n; i++) {
    if (ys[i] < ymin) ymin = ys[i];
    if (ys[i] > ymax) ymax = ys[i];
    if (xs[i] < xmin) xmin = xs[i];
    if (xs[i] > xmax) xmax = xs[i];
  }
  int y0 = (int)floorf(ymin), y1 = (int)ceilf(ymax);
  int x0 = (int)floorf(xmin), x1 = (int)ceilf(xmax);
  if (y0 < gClipY0[cid]) y0 = gClipY0[cid];
  if (y1 > gClipY1[cid]) y1 = gClipY1[cid];
  if (x0 < 0) x0 = 0;
  if (x1 > FB_W) x1 = FB_W;
  if (y0 >= y1 || x0 >= x1) return;

  float gdx = p.gx1 - p.gx0, gdy = p.gy1 - p.gy0;
  float glen2 = gdx * gdx + gdy * gdy;
  if (glen2 < 1e-9f) glen2 = 1e-9f;
  const float invGlen2 = 1.0f / glen2;      // hoisted: float divide is a
                                            // library call on Xtensa
  const int SUB = 2;   // vertical sub-scanlines; x coverage is exact anyway
  const float INV_SUB = 1.0f / SUB;
  float cx[16];
  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) covRow[x] = 0;
    bool any = false;
    for (int s = 0; s < SUB; s++) {
      float sy = y + (s + 0.5f) * INV_SUB;
      int nc = 0;
      for (int i = 0; i < n && nc < 16; i++) {
        int j = (i + 1) % n;
        float ay = ys[i], by = ys[j];
        if (ay == by) continue;
        if ((sy >= ay && sy < by) || (sy >= by && sy < ay)) {
          float t = (sy - ay) / (by - ay);
          cx[nc++] = xs[i] + (xs[j] - xs[i]) * t;
        }
      }
      if (nc < 2) continue;
      for (int a = 1; a < nc; a++) {
        float k = cx[a]; int b = a - 1;
        while (b >= 0 && cx[b] > k) { cx[b + 1] = cx[b]; b--; }
        cx[b + 1] = k;
      }
      for (int a = 0; a + 1 < nc; a += 2) {
        float xa = cx[a], xb = cx[a + 1];
        if (xb <= x0 || xa >= x1) continue;
        if (xa < x0) xa = x0;
        if (xb > x1) xb = x1;
        int ia = (int)floorf(xa), ib = (int)ceilf(xb);
        for (int x = ia; x < ib && x < x1; x++) {
          if (x < x0) continue;
          float l = fmaxf(xa, (float)x);
          float r = fminf(xb, (float)x + 1);
          if (r > l) { covRow[x] += (r - l) * INV_SUB; any = true; }
        }
      }
    }
    if (!any) continue;
    for (int x = x0; x < x1; x++) {
      float cov = covRow[x];
      if (cov <= 0.002f) continue;
      if (cov > 1) cov = 1;
      float rr, gg, bb, aa;
      if (p.grad) {
        float t = ((x + 0.5f - p.gx0) * gdx + (y + 0.5f - p.gy0) * gdy) * invGlen2;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        rr = p.r0 + (p.r1 - p.r0) * t;
        gg = p.g0 + (p.g1 - p.g0) * t;
        bb = p.b0 + (p.b1 - p.b0) * t;
        aa = p.a0 + (p.a1 - p.a0) * t;
      } else {
        rr = p.r0; gg = p.g0; bb = p.b0; aa = p.a0;
      }
      aa *= cov;
      if (aa <= 0.002f) continue;
      if (p.additive) {
        px_add(x, y, (int)(rr * aa * (31.0f / 255.0f) + 0.5f),
                     (int)(gg * aa * (63.0f / 255.0f) + 0.5f),
                     (int)(bb * aa * (31.0f / 255.0f) + 0.5f));
      } else {
        px_blend(x, y, rgb565((int)rr, (int)gg, (int)bb), (int)(aa * 32 + 0.5f));
      }
    }
  }
}

void fillRectAA(float x, float y, float w, float h, uint16_t c, float a) {
  if (w <= 0 || h <= 0 || a <= 0) return;
  const int cid = xPortGetCoreID();
  int ix0 = (int)floorf(x), ix1 = (int)ceilf(x + w);
  int iy0 = (int)floorf(y), iy1 = (int)ceilf(y + h);
  if (ix0 < 0) ix0 = 0;
  if (iy0 < gClipY0[cid]) iy0 = gClipY0[cid];
  if (ix1 > FB_W) ix1 = FB_W;
  if (iy1 > gClipY1[cid]) iy1 = gClipY1[cid];
  for (int j = iy0; j < iy1; j++) {
    float cy = fminf(y + h, (float)j + 1) - fmaxf(y, (float)j);
    if (cy <= 0) continue;
    for (int i = ix0; i < ix1; i++) {
      float cx = fminf(x + w, (float)i + 1) - fmaxf(x, (float)i);
      if (cx <= 0) continue;
      int al = (int)(a * cx * cy * 32 + 0.5f);
      if (al > 0) px_blend(i, j, c, al);
    }
  }
}

void lineAA(float x0, float y0, float x1, float y1, float w, uint16_t c, float a) {
  float dx = x1 - x0, dy = y1 - y0;
  float l = sqrtf(dx * dx + dy * dy);
  if (l < 1e-4f) return;
  float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
  float xs[4] = { x0 + nx, x1 + nx, x1 - nx, x0 - nx };
  float ys[4] = { y0 + ny, y1 + ny, y1 - ny, y0 - ny };
  PolyPaint p;
  p.r0 = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
  p.g0 = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
  p.b0 = (uint8_t)((c & 0x1F) * 255 / 31);
  p.a0 = a;
  fillPolyAA(xs, ys, 4, p);
}

void circleOutlineAA(float cx, float cy, float r, float lw, uint16_t c, float a) {
  float ro = r + lw * 0.5f;
  const int cid = xPortGetCoreID();
  int x0 = (int)floorf(cx - ro - 1), x1 = (int)ceilf(cx + ro + 1);
  int y0 = (int)floorf(cy - ro - 1), y1 = (int)ceilf(cy + ro + 1);
  if (x0 < 0) x0 = 0;
  if (y0 < gClipY0[cid]) y0 = gClipY0[cid];
  if (x1 > FB_W) x1 = FB_W;
  if (y1 > gClipY1[cid]) y1 = gClipY1[cid];
  const float half = lw * 0.5f;
  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) {
      // 2x2 samples keep the 1-2px bubble rings from popping
      float cov = 0;
      for (int sy = 0; sy < 2; sy++) {
        for (int sx = 0; sx < 2; sx++) {
          float px = x + 0.25f + sx * 0.5f;
          float py = y + 0.25f + sy * 0.5f;
          float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
          if (fabsf(d - r) <= half) cov += 0.25f;
        }
      }
      if (cov <= 0) continue;
      int al = (int)(a * cov * 32 + 0.5f);
      if (al > 0) px_blend(x, y, c, al);
    }
  }
}

// ---------------------------------------------------------------------------
// 3x5 glyphs: 0-9, 'c', 'm' - the depth rail is the only text in the tank
static const uint8_t GLYPH[12][5] = {
  { 0b111, 0b101, 0b101, 0b101, 0b111 }, // 0
  { 0b010, 0b110, 0b010, 0b010, 0b111 }, // 1
  { 0b111, 0b001, 0b111, 0b100, 0b111 }, // 2
  { 0b111, 0b001, 0b111, 0b001, 0b111 }, // 3
  { 0b101, 0b101, 0b111, 0b001, 0b001 }, // 4
  { 0b111, 0b100, 0b111, 0b001, 0b111 }, // 5
  { 0b111, 0b100, 0b111, 0b101, 0b111 }, // 6
  { 0b111, 0b001, 0b001, 0b010, 0b010 }, // 7
  { 0b111, 0b101, 0b111, 0b101, 0b111 }, // 8
  { 0b111, 0b101, 0b111, 0b001, 0b111 }, // 9
  { 0b000, 0b111, 0b100, 0b100, 0b111 }, // c
  { 0b000, 0b111, 0b111, 0b101, 0b101 }, // m
};

void fb_label(int x, int y, const char* s, uint16_t c, float a) {
  int al = (int)(a * 32 + 0.5f);
  if (al <= 0) return;
  for (const char* p = s; *p; p++) {
    int gi = -1;
    if (*p >= '0' && *p <= '9') gi = *p - '0';
    else if (*p == 'c') gi = 10;
    else if (*p == 'm') gi = 11;
    if (gi >= 0) {
      for (int r = 0; r < 5; r++)
        for (int b = 0; b < 3; b++)
          if (GLYPH[gi][r] & (1 << (2 - b))) px_blend(x + b, y + r, c, al);
    }
    x += 4;
  }
}

void IRAM_ATTR fbSwapBand(int y0, int y1) {
  uint32_t* p = (uint32_t*)(FB + (size_t)y0 * FB_W);
  size_t n = (size_t)(y1 - y0) * FB_W / 2;      // two pixels per word
  for (size_t i = 0; i < n; i++) {
    uint32_t v = p[i];
    p[i] = ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
  }
}
