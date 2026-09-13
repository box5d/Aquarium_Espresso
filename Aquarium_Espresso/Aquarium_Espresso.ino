// ===========================================================================
// Aquarium_Espresso — ESP32-S3 + ST7789 (320x240) port of the `pixel-aquarium`
// web app (g:/マイドライブ/Arduino/app).
//
// 33 fish in four species, each with its own habits and its own layer of the
// water column: guppies up top, black tetras hanging in the middle, three
// shoals of neon tetras below them, corydoras working the sand. Bodies are
// spine chains drawn as bent sprite strips.
//
// Everything the web build drew around the fish is replaced by a photo of a
// real tank: one of the five shots in app/ is picked at random each boot,
// decoded into PSRAM and lit every frame by light.cpp - LED inverter flicker,
// surface caustics and the refraction shimmer of water in motion.
//
// And if there is an SD card with a fish.png on it, one to three copies of
// whatever is drawn on it swim here too - see cardfish.h.
//
// Wiring / panel options: lgfx_setup.h
// Backdrop photos:        bg_images.cpp (regenerate with tools/make_bg.py)
// SD card / the drawing:  cardfish.cpp
// ===========================================================================
#include <esp_heap_caps.h>
#include "lgfx_setup.h"
#include "gfx.h"
#include "rig.h"
#include "sim.h"
#include "renderer.h"
#include "light.h"
#include "bubbles.h"
#include "cardfish.h"
#include "fastmath.h"
#include "bg_images.h"

extern uint32_t tRestore[2], tVeil[2], tSeg[2], tExtra[2];

static LGFX tft;
static Sim  sim;

// There is no tap input on this build, so the tank startles itself now and
// then to keep the dart / turn / flare behaviour visible.
static float autoTap = 4.0f;

// Bands the frame is composed and DMA'd in. The SPI peripheral only starts a
// transfer asynchronously when it fits one hardware descriptor run (32KB); a
// larger one falls back to a blocking chunk loop. 320x48x2 = 30720 B stays
// under that, so five bands is the coarsest split that actually overlaps.
static const int BANDS = 5;

// ---------------------------------------------------------------------------
// Both cores draw.
//
// The frame is panel-bound: 320x240 at 16 bits and 20MHz is 61.4ms on the wire,
// and nothing can make a frame shorter than that. What the CPU can do is stay
// out of the way - every microsecond it spends drawing while the bus is idle is
// a microsecond added on top of those 61.4.
//
// Two things were costing exactly that. The first band of a frame had no
// transfer to hide behind, and all the drawing sat on core 1 while core 0 did
// nothing whatsoever. So: band 0 is now drawn during the *previous* frame's
// last transfer, and every band is split between the two cores - core 0 takes
// the top slice, core 1 the bottom, and core 1 joins before handing the band to
// the DMA. Between them that is enough to fit all the drawing inside the wire
// time, which puts the frame rate on the panel's own ceiling.
//
// Everything a core touches while drawing has to be its own: the band clip
// (gfx.h), the polygon coverage row (gfx.cpp), the broad-wave row (light.cpp)
// and the timing counters (renderer.cpp) are all per core for this reason.
static const float SPLIT = 0.50f;        // share of a band core 0 takes

static TaskHandle_t hWorker = nullptr;
static TaskHandle_t hMain   = nullptr;
static const Sim*   wSim    = nullptr;
static volatile int wY0 = 0, wY1 = 0;

static void renderWorker(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    renderBand(*wSim, wY0, wY1);
    xTaskNotifyGive(hMain);
  }
}

// Draw one band on both cores and return when it is whole.
static void renderBandMT(const Sim& s, int y0, int y1) {
  const int ym = y0 + (int)((y1 - y0) * SPLIT + 0.5f);
  wSim = &s;
  wY0 = y0;
  wY1 = ym;
  xTaskNotifyGive(hWorker);
  renderBand(s, ym, y1);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static bool loadBackdrop() {
  BGBUF = (uint16_t*)heap_caps_malloc((size_t)FB_W * FB_H * sizeof(uint16_t),
                                      MALLOC_CAP_SPIRAM);
  if (!BGBUF) {
    Serial.println("FATAL: no PSRAM for the backdrop");
    return false;
  }
  LGFX_Sprite spr(&tft);
  spr.setPsram(true);
  spr.setColorDepth(16);
  if (!spr.createSprite(FB_W, FB_H)) {
    Serial.println("FATAL: backdrop sprite alloc failed");
    return false;
  }
  const int pick = (int)(esp_random() % N_BG_IMAGES);
  spr.fillScreen(0);
  bool ok = spr.drawJpg(BG_IMAGES[pick].data, BG_IMAGES[pick].len, 0, 0);
  // read back through an explicit rgb565 type so the byte order matches FB
  spr.readRect(0, 0, FB_W, FB_H, (lgfx::rgb565_t*)BGBUF);
  spr.deleteSprite();
  Serial.printf("backdrop %d/%d (%u B) decode %s\n",
                pick + 1, N_BG_IMAGES, (unsigned)BG_IMAGES[pick].len,
                ok ? "ok" : "FAILED");
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\npixel-aquarium / ESP32-S3 + ST7789");

  fastMathInit();

  tft.init();
  tft.setRotation(1);              // 240x320 panel -> 320x240 landscape
  tft.setColorDepth(16);
  tft.fillScreen(0);
#if PIN_TFT_BLK >= 0
  pinMode(PIN_TFT_BLK, OUTPUT);
  digitalWrite(PIN_TFT_BLK, HIGH);
#endif

  const size_t fbBytes = (size_t)FB_W * FB_H * sizeof(uint16_t);
  FB = (uint16_t*)heap_caps_malloc(fbBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!FB) {
    FB = (uint16_t*)heap_caps_malloc(fbBytes, MALLOC_CAP_SPIRAM);
    Serial.println("framebuffer: PSRAM fallback");
  }
  if (!FB) {
    Serial.println("FATAL: no framebuffer");
    while (true) delay(1000);
  }
  Serial.printf("framebuffer %ux%u (%u B), free internal %u\n",
                FB_W, FB_H, (unsigned)fbBytes,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  if (!loadBackdrop()) while (true) delay(1000);
  lightInit();
  lightPrepBackdrop();
  bubblesInit();

  // Before buildRigs(), which is where the sprite pointers are wired up. No
  // card, no card file, or an unreadable one, and the tank simply comes up
  // without them - this is a thing the tank can have, not a thing it needs.
  if (cardLoad()) {
    CARDFISH.count = CARD_MIN + (int)(esp_random() % (CARD_MAX - CARD_MIN + 1));
  }

  uint32_t t0 = millis();
  if (!buildRigs()) {
    Serial.println("FATAL: sprite bake failed");
    while (true) delay(1000);
  }
  Serial.printf("sprites baked in %lu ms\n", (unsigned long)(millis() - t0));

  makeSim(sim);

  hMain = xTaskGetCurrentTaskHandle();
  // core 0 is otherwise idle on this build - no radio, no file system - so the
  // worker owns it outright
  xTaskCreatePinnedToCore(renderWorker, "render0", 8192, nullptr, 2,
                          &hWorker, 0);
  Serial.printf("drawing on core %d + core 0\n", xPortGetCoreID());
}

void loop() {
  static uint32_t last = micros();
  static uint32_t fpsT = millis();
  static int frames = 0;

  uint32_t now = micros();
  float dt = (now - last) / 1000000.0f;
  last = now;
  if (dt > 1.0f / 30) dt = 1.0f / 30;

  autoTap -= dt;
  if (autoTap <= 0) {
    autoTap = 12.0f + (float)esp_random() / 4294967296.0f * 20.0f;
    float tx = 20 + (float)esp_random() / 4294967296.0f * 280.0f;
    float ty = VIEW::y0 + (float)esp_random() / 4294967296.0f * (VIEW::y1 - VIEW::y0);
    tapWater(sim, tx, ty);
  }

  static uint32_t accSim = 0, accDraw = 0, accPush = 0, accEnd = 0;

  static bool writeOpen = false;

  uint32_t m0 = micros();
  bubblesStep(dt);
  lightStep(dt);
  stepSim(sim, dt);

  // The last band of the previous frame is on the wire through all of this, and
  // there is enough of it left over to draw the first band of the new frame as
  // well - the one band that would otherwise have no transfer to hide behind.
  renderBandMT(sim, 0, SCR_H / BANDS);
  uint32_t m1 = micros();

  // Only now wait for that last band, which by this point has little left to
  // run.
  if (writeOpen) { tft.endWrite(); writeOpen = false; }
  uint32_t m1b = micros();

  // Send band by band: each pushImageDMA() returns while the panel is still
  // being fed, so the next band is drawn during the transfer.
  tft.startWrite();
  writeOpen = true;
  for (int b = 0; b < BANDS; b++) {
    const int y0 = b * SCR_H / BANDS;
    const int y1 = (b + 1) * SCR_H / BANDS;
    uint32_t p0 = micros();
    fbSwapBand(y0, y1);
    tft.pushImageDMA(0, y0, FB_W, y1 - y0,
                     (lgfx::swap565_t*)(FB + (size_t)y0 * FB_W));
    accPush += micros() - p0;
    if (b + 1 < BANDS)
      renderBandMT(sim, y1, (b + 2) * SCR_H / BANDS);
  }
  uint32_t m2 = micros();
  accSim += m1 - m0;
  accDraw += m2 - m1b;
  accEnd += m1b - m1;

  frames++;
  if (millis() - fpsT >= 5000) {
    uint32_t el = millis() - fpsT;
    Serial.printf("%.1f fps sim+b0 %.2f draw %.2f (cpu bg %.2f veil %.2f seg %.2f fin %.2f) dma %.2f end %.2f\n",
                  frames * 1000.0f / el,
                  accSim / 1000.0f / frames,
                  accDraw / 1000.0f / frames,
                  (tRestore[0] + tRestore[1]) / 1000.0f / frames,
                  (tVeil[0] + tVeil[1]) / 1000.0f / frames,
                  (tSeg[0] + tSeg[1]) / 1000.0f / frames,
                  (tExtra[0] + tExtra[1]) / 1000.0f / frames,
                  accPush / 1000.0f / frames,
                  accEnd / 1000.0f / frames);
    tRestore[0] = tRestore[1] = tVeil[0] = tVeil[1] = 0;
    tSeg[0] = tSeg[1] = tExtra[0] = tExtra[1] = 0;
    fpsT = millis();
    frames = 0;
    accSim = accDraw = accPush = accEnd = 0;
  }
}
