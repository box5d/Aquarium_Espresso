#include "rig.h"
#include "cardfish.h"

// ---------------------------------------------------------------------------
// Water layers. A community tank stratifies - that is most of what stops it
// reading as one soup of fish - so each species gets the band it actually uses,
// in panel rows. The swim area runs from SWIM_TOP (30) to SWIM_BOT (205); see
// sim.h. Neighbouring bands overlap a little on purpose.
//
//   guppy        30 .. 118   upper to middle
//   black tetra  84 .. 142   middle, and it barely leaves it
//   neon tetra  100 .. 185   middle to lower
//   amano shrimp 188 .. 207  the sand, a shade nearer than the corydoras
//   corydoras   188 .. 205   on the sand
//
// ---------------------------------------------------------------------------
// Sizes come from each photo's own aspect ratio (tools/make_fish.py prints
// them), so the long-finned strains really are taller than the short-tailed
// one. At 24px for a 4.5cm guppy the scale is 5.3 px/cm, which puts a 3cm neon
// tetra at 16px.
const SpeciesCfg NEON = {
  SP_NEON, "NEON TETRA", 20, 3.0f,
  12, 5, 12.0f / 4,
  14.0f, 4.2f, 3.1f,
  { 0, 0.25f, 0.60f, 1.15f, 1.80f },
  8.7f, 0.15f, 0.0f, 100.0f, 185.0f, 0.30f, 0, NEON_ART,
};

const SpeciesCfg GUPPY_STRAINS[5] = {
  { SP_GUPPY, "GUPPY", 2, 4.5f, 24, 13, 24.0f / 4, 9.0f, 2.4f, 2.3f,
    { 0, 0.18f, 0.60f, 1.4f, 2.8f }, 15.2f, 0.55f, 0.0f, 30.0f, 118.0f, 0.38f, 0, GUPPY1_ART },
  { SP_GUPPY, "GUPPY", 2, 4.5f, 24, 12, 24.0f / 4, 9.0f, 2.4f, 2.3f,
    { 0, 0.18f, 0.60f, 1.4f, 2.8f }, 15.0f, 0.55f, 0.0f, 30.0f, 118.0f, 0.38f, 1, GUPPY2_ART },
  { SP_GUPPY, "GUPPY", 2, 4.5f, 24, 12, 24.0f / 4, 9.0f, 2.4f, 2.3f,
    { 0, 0.18f, 0.60f, 1.4f, 2.8f }, 14.3f, 0.55f, 0.0f, 30.0f, 118.0f, 0.38f, 2, GUPPY3_ART },
  { SP_GUPPY, "GUPPY", 1, 4.5f, 24, 10, 24.0f / 4, 9.0f, 2.4f, 2.3f,
    { 0, 0.18f, 0.60f, 1.4f, 2.8f }, 15.6f, 0.55f, 0.0f, 30.0f, 118.0f, 0.38f, 3, GUPPY4_ART },
  { SP_GUPPY, "GUPPY", 1, 4.5f, 24, 13, 24.0f / 4, 9.0f, 2.4f, 2.3f,
    { 0, 0.18f, 0.60f, 1.4f, 2.8f }, 15.3f, 0.55f, 0.0f, 30.0f, 118.0f, 0.38f, 4, GUPPY5_ART },
};

// Gymnocorymbus ternetzi. Deep, laterally flattened body with a big black
// anal-fin skirt. In a group of three - well under the shoal size the species
// wants - they do not shoal; they hang in mid-water, hold station for long
// stretches and only drift a short way between stops.
const SpeciesCfg BLACKTETRA = {
  SP_BLACK, "BLACK TETRA", 3, 5.0f,
  24, 14, 24.0f / 4,
  9.0f, 2.0f, 1.6f,
  { 0, 0.14f, 0.40f, 0.90f, 1.70f },
  18.1f, 0.35f, 70.0f, 84.0f, 142.0f, 0.30f, 0, BLACK_ART,
};

// Armoured catfish: works the substrate in short scoots with long pauses, and
// every minute or two bolts to the surface for a gulp of air (it breathes
// through its gut) before settling quietly back down. baseSpeed is the dash
// speed - the crawl comes from a very low effort, so both extremes fit in one
// scale.
const SpeciesCfg CORYDORAS = {
  SP_CORY, "CORYDORAS", 2, 5.0f,
  24, 14, 24.0f / 4,
  26.0f, 3.0f, 2.6f,
  { 0, 0.12f, 0.35f, 0.80f, 1.50f },
  17.7f, 0.25f, 90.0f, 188.0f, 205.0f, 0.25f, 0, CORY_ART,
};

// Caridina multidentata. Not a fish at all, and it is the one thing in the tank
// that mostly is not swimming: an Amano spends its day walking over the
// substrate and the hardscape picking algae off it with its front claws, in
// short scoots with long stops, and only pushes off and swims - level, on its
// pleopods, not on a tail - when it wants to be somewhere else. Startle it and
// it does the one thing that makes a shrimp unmistakable: flicks its abdomen
// and shoots *backwards*.
//
// Females reach 5-6cm and males 3.5-4.5cm. This one is sized as a male rather
// than a female: a shrimp is not the thing you are meant to be looking at, and
// at female size it read as a pale object sitting on the photograph rather than
// as something living on the sand. It is also baked translucent, because that
// is what the animal is.
//
// The body is nearly rigid - almost all of a shrimp's flex is in the abdomen
// and only during that escape flick - so A[] is a fraction of what a tetra gets
// and the tail beat is really the swimmerets ticking over.
//
// The sprite is the body only. Its antennae are as long as it is and would
// have eaten a fifth of the sprite width for two hairs that disappear at this
// size, so they are cropped out of the bake and drawn in renderer.cpp instead.
const SpeciesCfg YAMATO = {
  SP_SHRIMP, "AMANO SHRIMP", 5, 4.0f,
  24, 8, 24.0f / 4,
  16.0f, 3.6f, 5.0f,
  { 0, 0.05f, 0.12f, 0.30f, 0.62f },
  19.7f, 0.10f, 60.0f, 188.0f, 207.0f, 0.18f, 0, SHRIMP_ART,
};

// Once per boot, every guppy in the tank becomes this and stays that way. It
// is authored at the guppy's own 24px width so the bone spacing is unchanged
// and the spine chain carries straight through the swap, but everything else
// says "fried": almost no body wave, no fin flare, and a slow stiff cruise.
const SpeciesCfg EBIFRY = {
  SP_EBI, "EBI FRY", 0, 4.5f,
  24, 9, 24.0f / 4,
  7.0f, 1.6f, 1.2f,
  { 0, 0.05f, 0.15f, 0.35f, 0.70f },
  19.0f, 0.05f, 0.0f, 30.0f, 118.0f, 0.20f, 0, EBIFRY_ART,
};

// The card fish. `count` and `art` are filled in at boot from whatever is on
// the SD card, and stay at 0/nullptr when there is no card - see cardfish.cpp.
//
// It is given the water the fish use rather than a layer of its own: a drawing
// belongs in the middle of the tank where you can see it, not tucked into a
// band. A[] is never read, because the card renderer does not use the spine.
//
// baseSpeed and turnRate are not free choices. Their ratio is the turning
// radius, and for a rigid card that radius is the whole difference between
// coming about and spinning on the spot: at the 11px/s and 1.7rad/s this
// started with, the radius was six pixels - a fifth of the card's own length,
// so it pivoted inside itself and read as a turntable. 17 and 0.70 put it at
// about 24px, three quarters of a body length, which is a sweeping arc you can
// watch it travel through. Slower and it cannot get out of a corner; faster
// and the paper is darting, which paper does not do.
SpeciesCfg CARDFISH = {
  SP_CARD, "CARD FISH", 0, 5.0f,
  CARD_W, CARD_H, (float)CARD_W / (BONES - 1),
  17.0f, 0.70f, 0.85f,
  { 0, 0, 0, 0, 0 },
  0.0f, 0.0f, 0.0f, 48.0f, 158.0f, 0.20f, 0, nullptr,
};

Rig RIGS[N_RIGS];

const Rig* rigFor(const SpeciesCfg* cfg) {
  for (int i = 0; i < N_RIGS; i++) if (RIGS[i].cfg == cfg) return &RIGS[i];
  return &RIGS[0];
}

bool buildRigs() {
  RIGS[0].cfg = &NEON;
  RIGS[0].spr = NEON.art;
  for (int i = 0; i < 5; i++) {
    RIGS[i + 1].cfg = &GUPPY_STRAINS[i];
    RIGS[i + 1].spr = GUPPY_STRAINS[i].art;
  }
  RIGS[6].cfg = &BLACKTETRA;  RIGS[6].spr = BLACKTETRA.art;
  RIGS[7].cfg = &CORYDORAS;   RIGS[7].spr = CORYDORAS.art;
  RIGS[8].cfg = &YAMATO;      RIGS[8].spr = YAMATO.art;
  RIGS[9].cfg = &EBIFRY;      RIGS[9].spr = EBIFRY.art;
  CARDFISH.art = CARD_ART;    // still nullptr if there was no usable card
  RIGS[10].cfg = &CARDFISH;   RIGS[10].spr = CARDFISH.art;
  return true;
}
