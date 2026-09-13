#ifndef PIXAQ_RIG_H
#define PIXAQ_RIG_H
// ---------------------------------------------------------------------------
// rig.h — species definitions and the sprite each one is drawn from.
//
// The browser build stacks five vertically-offset copies of the same art into
// a sheet and samples a shifted block to fake body roll. Working through that
// indexing, block `bias` sampled at row r always resolves to art row r - bias,
// so one sprite is enough here: the renderer offsets the source row and treats
// out-of-range rows as transparent.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include "fish_art.h"
#include "gfx.h"

static const int BONES = 5;

enum SpKey : uint8_t { SP_NEON = 0, SP_GUPPY = 1, SP_BLACK = 2, SP_CORY = 3,
                       SP_EBI = 4, SP_SHRIMP = 5, SP_CARD = 6 };

struct SpeciesCfg {
  SpKey       key;
  const char* label;
  int         count;
  float       lenCm;
  int         W, H;        // sprite size; W must be a multiple of BONES-1
  float       spacing;     // spine bone spacing (px) = W / (BONES-1)
  float       baseSpeed;   // px/s
  float       turnRate;    // rad/s
  float       beatHz;      // tail beats per second at cruise
  float       A[BONES];    // lateral amplitude per bone (px)
  float       finFrom;     // arc distance behind the head where the fin starts
  float       fanRipple;   // how much the fin surface ripples across its height
  float       roam;        // wander radius (px); 0 = the whole tank
  float       yLo, yHi;    // the layer of water this species lives in, in rows
  float       pitchMax;    // max pseudo-pitch during depth events
  uint8_t     strain;      // colour variant, for picking the art
  const RGBA8* art;
};

struct Rig {
  const SpeciesCfg* cfg;
  const RGBA8*      spr;   // cfg->W * cfg->H, straight-on art, head at +x
};

extern const SpeciesCfg NEON;
extern const SpeciesCfg GUPPY_STRAINS[5];
extern const SpeciesCfg BLACKTETRA;
extern const SpeciesCfg CORYDORAS;
extern const SpeciesCfg YAMATO;
extern const SpeciesCfg EBIFRY;
// Not const: how many there are and what they look like are both decided at
// boot, by what is on the card. See cardfish.h.
extern SpeciesCfg CARDFISH;
static const int N_RIGS = 11;         // five species, five guppy strains, gag,
                                      // and whatever is on the SD card

extern Rig RIGS[N_RIGS];

bool buildRigs();                     // wire the sprites up; call from setup()
const Rig* rigFor(const SpeciesCfg* cfg);

#endif // PIXAQ_RIG_H
