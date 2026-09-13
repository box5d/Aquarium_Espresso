#ifndef PIXAQ_GFX_H
#define PIXAQ_GFX_H
// ---------------------------------------------------------------------------
// gfx.h — RGB565 framebuffer primitives.
//
// The browser build composites everything through canvas 2D with float alpha.
// Here the scene is composed into one RGB565 buffer with 5-bit alpha blends
// (the classic split-field trick), then pushed to the panel in one SPI burst.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <math.h>

// --- tank framebuffer geometry ---------------------------------------------
// The whole panel is the tank: a photo backdrop with the fish composited over
// it, so the framebuffer covers all 320x240 and is pushed as one image.
static const int SCR_W = 320;
static const int SCR_H = 240;
static const int FB_W = SCR_W;
static const int FB_H = SCR_H;
static const int OFF_Y = 0;

extern uint16_t* FB;      // composite target, internal RAM
extern uint16_t* BGBUF;   // decoded tank photo, PSRAM

// --- dirty box ---------------------------------------------------------------
// Every write goes through px_blend()/px_add(), so accumulating the touched box
// there is exact: the box a fish reports always contains every pixel it wrote.
// That is what lets the next frame restore only those rows from the backdrop
// and push only those rows to the panel.
extern int gDX0, gDY0, gDX1, gDY1;

// Row band currently being composed, per core. The frame is rendered and sent
// in bands so one band's DMA overlaps the next band's drawing, and each band is
// split again between the two cores - so the clip is the one piece of drawing
// state that cannot be global. Clamping loop bounds to it costs nothing per
// pixel; it is only ever read when a shape sets up its span loops.
extern int gClipY0[2], gClipY1[2];
static inline void clipBand(int y0, int y1) {
  const int c = xPortGetCoreID();
  gClipY0[c] = y0;
  gClipY1[c] = y1;
}

static inline void dirtyReset() { gDX0 = FB_W; gDY0 = FB_H; gDX1 = 0; gDY1 = 0; }
static inline void dirtyMark(int x, int y) {
  if (x < gDX0) gDX0 = x;
  if (x >= gDX1) gDX1 = x + 1;
  if (y < gDY0) gDY0 = y;
  if (y >= gDY1) gDY1 = y + 1;
}

static constexpr uint16_t rgb565(int r, int g, int b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// alpha 0..32
static inline uint16_t blend565(uint16_t dst, uint16_t src, uint32_t a) {
  if (a >= 32) return src;
  if (a == 0) return dst;
  uint32_t ia = 32 - a;
  uint32_t drb = dst & 0xF81F, dg = dst & 0x07E0;
  uint32_t srb = src & 0xF81F, sg = src & 0x07E0;
  uint32_t orb = ((srb * a + drb * ia) >> 5) & 0xF81F;
  uint32_t og  = ((sg  * a + dg  * ia) >> 5) & 0x07E0;
  return (uint16_t)(orb | og);
}

static inline uint16_t add565(uint16_t dst, int r, int g, int b) {
  int dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
  dr += r; dg += g; db += b;
  if (dr > 31) dr = 31;
  if (dg > 63) dg = 63;
  if (db > 31) db = 31;
  return (uint16_t)((dr << 11) | (dg << 5) | db);
}

static inline void px_blend(int x, int y, uint16_t c, int a) {
  if ((unsigned)x >= (unsigned)FB_W || (unsigned)y >= (unsigned)FB_H) return;
  if (a <= 0) return;
  uint16_t* p = FB + y * FB_W + x;
  *p = blend565(*p, c, a > 32 ? 32 : (uint32_t)a);
  dirtyMark(x, y);
}

// Premultiplied source-over. The strip renderer accumulates colour already
// multiplied by coverage, so blending it directly avoids un-premultiplying -
// three float divides per pixel that cost more than everything around them.
// pr/pg/pb are 5/6/5-bit premultiplied, a is 0..32.
static inline void px_blend_pm(int x, int y, int pr, int pg, int pb, int a) {
  if ((unsigned)x >= (unsigned)FB_W || (unsigned)y >= (unsigned)FB_H) return;
  uint16_t* p = FB + y * FB_W + x;
  uint16_t d = *p;
  int ia = 32 - a;
  int r = (((d >> 11) & 0x1F) * ia >> 5) + pr;
  int g = (((d >> 5) & 0x3F) * ia >> 5) + pg;
  int b = ((d & 0x1F) * ia >> 5) + pb;
  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (b > 31) b = 31;
  *p = (uint16_t)((r << 11) | (g << 5) | b);
  dirtyMark(x, y);
}

static inline void px_add(int x, int y, int r, int g, int b) {
  if ((unsigned)x >= (unsigned)FB_W || (unsigned)y >= (unsigned)FB_H) return;
  uint16_t* p = FB + y * FB_W + x;
  *p = add565(*p, r, g, b);
  dirtyMark(x, y);
}

static inline void fb_hline(int x0, int x1, int y, uint16_t c) {
  if ((unsigned)y >= (unsigned)FB_H) return;
  if (x0 < 0) x0 = 0;
  if (x1 > FB_W) x1 = FB_W;
  uint16_t* p = FB + y * FB_W + x0;
  for (int x = x0; x < x1; x++) *p++ = c;
}

static inline void fb_rect(int x, int y, int w, int h, uint16_t c) {
  for (int j = y; j < y + h; j++) fb_hline(x, x + w, j, c);
}

static inline void fb_rect_a(int x, int y, int w, int h, uint16_t c, int a) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) px_blend(i, j, c, a);
}

// --- anti-aliased primitives (3 sub-scanlines + exact span coverage) -------
struct PolyPaint {
  bool  grad = false;
  bool  additive = false;
  float gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
  uint8_t r0 = 255, g0 = 255, b0 = 255; float a0 = 1.0f;
  uint8_t r1 = 255, g1 = 255, b1 = 255; float a1 = 1.0f;
};

void fillPolyAA(const float* xs, const float* ys, int n, const PolyPaint& p);
void fillRectAA(float x, float y, float w, float h, uint16_t c, float a);
void lineAA(float x0, float y0, float x1, float y1, float w, uint16_t c, float a);
void circleOutlineAA(float cx, float cy, float r, float lw, uint16_t c, float a);

// Byte-swap a band in place, ready for the panel.
//
// LovyanGFX only starts a real asynchronous DMA when the source needs no
// conversion, which means the panel's own byte order (swap565). Handing it
// native rgb565 instead sends the frame through a converting path that keeps
// pace with the wire but blocks for the whole transfer. Compositing stays in
// native order - one swap pass over the finished band is far cheaper than
// carrying swapped pixels through every blend.
void fbSwapBand(int y0, int y1);

// tiny 3x5 label font - only the glyphs the depth rail needs (0-9, c, m)
void fb_label(int x, int y, const char* s, uint16_t c, float a);

#endif // PIXAQ_GFX_H
