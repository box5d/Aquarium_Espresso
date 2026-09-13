#pragma GCC optimize("O3")
#include "sim.h"
#include "fastmath.h"
#include "bubbles.h"
#include "cardfish.h"

static inline float rnd(float a, float b) {
  return a + (float)esp_random() / 4294967296.0f * (b - a);
}
static inline float rnd01() { return (float)esp_random() / 4294967296.0f; }
static inline float clampf(float v, float a, float b) {
  return v < a ? a : (v > b ? b : v);
}
static inline float wrapAng(float a) { return fwrap(a); }

static void makeFish(Fish& f, const SpeciesCfg* cfg, bool school, int sid,
                     float sx, float sy) {
  f.cfg = cfg;
  f.home = cfg;
  // spawn on the shoal it belongs to, or in the water this species uses, so
  // the tank never opens with every fish in one corner
  f.x = school ? sx + rnd(-30, 30) : rnd(22, 298);
  f.y = school ? sy + rnd(-16, 16) : rnd(cfg->yLo, cfg->yHi);
  f.x = clampf(f.x, VIEW::x0 + 4, VIEW::x1 - 4);
  f.y = clampf(f.y, cfg->yLo, cfg->yHi);
  f.heading = rnd(0, (float)M_PI * 2);
  f.trail.reset();
  // pre-fill a straight trail behind the head (0.5px spacing)
  for (int k = Trail::CAP - 1; k >= 0; k--) {
    f.trail.push(f.x - fcos(f.heading) * k * 0.5f,
                 f.y - fsin(f.heading) * k * 0.5f);
  }
  for (int k = 0; k < BONES; k++) {
    f.bones[k].x = f.x - fcos(f.heading) * k * cfg->spacing;
    f.bones[k].y = f.y - fsin(f.heading) * k * cfg->spacing;
    f.bones[k].a = f.heading;
  }
  f.speed = cfg->baseSpeed;
  f.prevHeading = f.heading;
  f.turnRate = 0;
  f.tx = rnd(30, 290);
  f.ty = rnd(VIEW::ymap(30), VIEW::ymap(110));
  f.retarget = rnd(2, 6);
  f.beat = rnd(0, 10);
  f.phase = rnd(0, (float)M_PI * 2);
  f.sf = rnd(0.88f, 1.12f);
  f.depth = { 0, 0, 1, 1, 1, 1, rnd(1, 5), 0 };
  f.biasF = 0; f.burstX = 0; f.burstY = 0; f.lifted = 0;
  f.sepX = 0; f.sepY = 0;
  f.speedNorm = 0.5f;
  f.school = school;
  f.sid = (uint8_t)(sid < 0 ? 0 : sid);
  f.orbitR = rnd(10, 34);
  f.facing = 1;
  f.mirror = (fcos(f.heading) < 0) ? -1.0f : 1.0f;
  f.turn = (f.mirror < 0) ? (float)M_PI : 0.0f;   // card only; see stepFish
  f.flare = 0;

  // guppies are the ones you actually catch loafing or picking at the surface;
  // tetras mostly stay with the shoal, but not always
  float r = rnd01();
  if (cfg->key == SP_BLACK)      f.pers = PERS_HOVERER;
  else if (cfg->key == SP_CORY)  f.pers = PERS_NONE;
  else if (cfg->key == SP_SHRIMP) f.pers = PERS_NONE;
  else if (cfg->key == SP_GUPPY) f.pers = r < 0.32f ? PERS_LOAFER
                                        : r < 0.62f ? PERS_GULPER : PERS_NONE;
  else                           f.pers = r < 0.08f ? PERS_LOAFER
                                        : r < 0.20f ? PERS_GULPER : PERS_NONE;
  f.act = (cfg->key == SP_CORY)   ? ACT_GRAZE
        : (cfg->key == SP_SHRIMP) ? ACT_PICK : ACT_SWIM;
  f.actT = rnd(1, 4);
  // Staggered, so they never all break at once. For the corydoras this is the
  // countdown to its next trip to the surface: measured air-breathing rates
  // run anywhere from 1 to 45 times an hour.
  // For the corydoras this is the countdown to its next trip to the surface;
  // for the shrimp, to its next proper swim rather than another scoot.
  f.nextAct = (cfg->key == SP_CORY)   ? rnd(10, 90)
            : (cfg->key == SP_SHRIMP) ? rnd(8, 45) : rnd(5, 30);
  f.holdX = f.x; f.holdY = f.y;
  f.effort = 1.0f;
  f.thrash = 0;
}

void makeSim(Sim& sim) {
  // the coins, tossed before anything else so the tank can be built already
  // fried and already stocked
  sim.ebiDay = (rnd01() < GAG_CHANCE);
  sim.shrimpDay = (rnd01() < SHRIMP_CHANCE);

  // Where the three shoals set up.
  //
  // These used to be constants - 70 / 160 / 250 across, and 20% / 50% / 80%
  // down - which meant every single boot opened with the same three shoals in
  // the same three places, in the same order, at the same depths. A tank that
  // looks identical every time you switch it on is the one thing this whole
  // build is trying not to be.
  //
  // So: the three stretches of the tank are handed out in a random order, each
  // shoal is jittered inside the stretch it drew, and its depth is drawn
  // separately from its position. Shuffling the stretches rather than just
  // jittering fixed ones matters - jitter alone still leaves the shallow shoal
  // on the left every time.
  int slot[N_SCHOOLS] = { 0, 1, 2 };
  for (int k = N_SCHOOLS - 1; k > 0; k--) {
    int j = (int)rnd(0, k + 1);
    if (j > k) j = k;                                // rnd() is inclusive at b
    int t = slot[k]; slot[k] = slot[j]; slot[j] = t;
  }
  for (int k = 0; k < N_SCHOOLS; k++) {
    float home = 62.0f + slot[k] * 98.0f + rnd(-24, 24);
    float y = NEON.yLo + (NEON.yHi - NEON.yLo) * rnd(0.12f, 0.86f);
    sim.school[k] = { home, y, home, y, rnd(2, 6), 0, home, y };
  }
  int i = 0;
  // On a shrimp day the shoal is cut right back rather than trimmed - that
  // emptiness in mid-water is what sends the eye down to the sand.
  const int nNeon = sim.shrimpDay ? N_NEON_SHRIMPDAY : NEON.count;
  for (int k = 0; k < nNeon; k++) {
    // Twenty tetras split into three shoals because one shoal of twenty packs
    // into a single blob. Five do not have that problem, and three shoals of
    // one or two fish read as strays rather than as a school - so on a shrimp
    // day they all stay together.
    int sid = sim.shrimpDay ? 1 : (k % N_SCHOOLS);
    makeFish(sim.fish[i++], &NEON, true, sid,
             sim.school[sid].x, sim.school[sid].y);
  }
  // The guppies, less one for every fish the card brought - and the ones that
  // lose their place are drawn at random rather than taken off the end, so it
  // is not always the same strain that goes missing.
  uint8_t gs[N_GUPPY];
  int ng = 0;
  for (int g = 0; g < 5; g++)
    for (int k = 0; k < GUPPY_STRAINS[g].count && ng < N_GUPPY; k++)
      gs[ng++] = (uint8_t)g;
  for (int d = 0; d < CARDFISH.count && ng > 0; d++) {
    int j = (int)rnd(0, ng);
    if (j >= ng) j = ng - 1;
    gs[j] = gs[--ng];
  }
  const int nGuppy = ng;
  for (int k = 0; k < ng; k++)
    makeFish(sim.fish[i++], &GUPPY_STRAINS[gs[k]], false, -1, 0, 0);
  for (int k = 0; k < BLACKTETRA.count; k++)
    makeFish(sim.fish[i++], &BLACKTETRA, false, -1, 0, 0);
  for (int k = 0; k < CORYDORAS.count; k++)
    makeFish(sim.fish[i++], &CORYDORAS, false, -1, 0, 0);
  if (sim.shrimpDay)
    for (int k = 0; k < YAMATO.count; k++)
      makeFish(sim.fish[i++], &YAMATO, false, -1, 0, 0);
  // and however many of somebody else's fish the card brought
  for (int k = 0; k < CARDFISH.count; k++)
    makeFish(sim.fish[i++], &CARDFISH, false, -1, 0, 0);
  sim.n = i;
  Serial.printf("%d fish: %d neons, %d guppies, %d amano shrimp, %d from the card\n",
                sim.n, nNeon, nGuppy, sim.shrimpDay ? YAMATO.count : 0,
                CARDFISH.count);

  // --- the joke ---
  // Decided here and nowhere else, so the tank never changes under the viewer.
  if (sim.ebiDay) {
    int n = 0;
    for (int k = 0; k < i; k++)
      if (sim.fish[k].home->key == SP_GUPPY) { sim.fish[k].cfg = &EBIFRY; n++; }
    Serial.printf("today the guppies (%d) are ebi-fry.\n", n);
  }

  for (int m = 0; m < N_MOTES; m++) {
    sim.motes[m] = { rnd(10, 310), rnd(VIEW::ymap(22), VIEW::ymap(128)),
                     rnd(-1.2f, 1.2f), rnd(-0.4f, 0.9f),
                     rnd(0.05f, 0.22f), rnd(0, 6) };
  }
  sim.nSurge = 0;
  sim.nBub = 0;
  sim.stress = 0;
  sim.nextBubble = 1.5f;
  sim.t = 0;
  sim.tw = 0;
  sim.sway = 0;
  sim.swayV = 0;
}

// --- depth event: fish swims toward / away from the viewer -----------------
void startDepthEvent(Fish& f, bool stress) {
  DepthEv& d = f.depth;
  if (d.mode == 1) return;
  // depth lunges happen only on a real startle; ambient swimming stays
  // strictly side-on
  if (!stress) { d.cool = 5; return; }
  const float maxD = 0.3f;
  float mag = (0.4f + rnd01() * 0.6f) * maxD;
  float sign = rnd01() < 0.5f ? -1.0f : 1.0f;
  // mid-turn the fish tends to nose toward the viewer
  if (fabsf(f.turnRate) > 1.2f && rnd01() < 0.65f) sign = -1;
  float to = clampf(f.sf + sign * mag, 0.74f, 1.28f);
  d.mode = 1; d.t = 0;
  d.dur = 0.45f + fabsf(to - f.sf) * 2.2f + rnd01() * 0.3f;
  d.from = f.sf; d.to = to;
  float s = to - f.sf;
  d.sign = (s > 0) ? 1.0f : (s < 0 ? -1.0f : 1.0f);
}

static void stepDepth(Fish& f, float dt) {
  DepthEv& d = f.depth;
  if (d.mode == 0) {
    d.cool -= dt;
    d.bell = fmaxf(0.0f, d.bell - dt * 3);
    if (d.cool <= 0) {
      startDepthEvent(f, false);
      d.cool = rnd(20, 40);
    }
    return;
  }
  d.t += dt;
  float p = clampf(d.t / d.dur, 0, 1);
  float e = p * p * (3 - 2 * p);              // smoothstep
  f.sf = d.from + (d.to - d.from) * e;
  d.bell = fsin((float)M_PI * p);             // transient 0->1->0
  if (p >= 1) { d.mode = 0; d.bell = 0; }
}

// --- spine chain -----------------------------------------------------------
void trailAt(const Trail& tr, float back, float& ox, float& oy) {
  int n = 0;                                   // 0 = newest
  float acc = 0;
  int i0 = tr.idx(0);
  float px = tr.x[i0], py = tr.y[i0];
  while (n < tr.n - 1 && acc < back) {
    int j = tr.idx(n + 1);
    float qx = tr.x[j], qy = tr.y[j];
    float dx = px - qx, dy = py - qy;
    float seg = sqrtf(dx * dx + dy * dy);
    if (acc + seg >= back) {
      float t = (back - acc) / (seg > 1e-6f ? seg : 1e-6f);
      ox = px + (qx - px) * t;
      oy = py + (qy - py) * t;
      return;
    }
    acc += seg; px = qx; py = qy; n++;
  }
  ox = px; oy = py;
}

// Body length that takes the full ride, cm. The neon tetra is the smallest
// thing in the tank, so it is the one that goes wherever the water goes.
static const float LIFT_REF_CM = 3.0f;

static void stepChain(Fish& f) {
  Trail& tr = f.trail;
  int last = tr.idx(0);
  float dx = f.x - tr.x[last], dy = f.y - tr.y[last];
  if (sqrtf(dx * dx + dy * dy) >= 0.45f) tr.push(f.x, f.y);

  float hx = fcos(f.heading), hy = fsin(f.heading);
  // tetras have stiff trunks; guppies flex a bit more
  const float kRelax = (f.cfg->key == SP_NEON) ? 0.5f : 0.45f;
  const float kAng   = (f.cfg->key == SP_NEON) ? 0.42f : 0.55f;
  for (int b = 0; b < BONES; b++) {
    float ptx, pty;
    trailAt(tr, b * f.cfg->spacing, ptx, pty);
    Bone& bone = f.bones[b];
    if (b == 0) {
      bone.a = f.heading;
    } else {
      float target = atan2f(f.bones[b - 1].y - pty, f.bones[b - 1].x - ptx);
      bone.a = fwrap(bone.a + wrapAng(target - bone.a) * kAng);
    }
    // relax each bone toward the straight line behind the head
    float sx = f.x - hx * b * f.cfg->spacing;
    float sy = f.y - hy * b * f.cfg->spacing;
    bone.x = ptx + (sx - ptx) * kRelax;
    bone.y = pty + (sy - pty) * kRelax;
  }
}

// --- individual habits -----------------------------------------------------
// Returns the effort this fish is putting into swimming, and takes over its
// steering target while an act is running.
static float stepAct(Sim& sim, Fish& f, float dt) {
  const float surfaceY = VIEW::y0 + 3;

  // A real startle always wins - a parked fish bolts like any other. Not for
  // the corydoras or the shrimp though: both have their own reaction below,
  // and for both `nextAct` counts down to a fixed habit rather than to a mood.
  if (sim.stress > 0.45f && f.act != ACT_SWIM
      && f.cfg->key != SP_CORY && f.cfg->key != SP_SHRIMP) {
    f.act = ACT_SWIM;
    f.nextAct = rnd(8, 20);
  }

  float want = 1.0f;
  float wantThrash = 0.0f;

  if (f.cfg->key == SP_SHRIMP) {
    // A shrimp is not a fish and must not move like one, and it is not the
    // thing you are meant to be watching. Almost all of its day is spent
    // stopped, working one patch of sand over with its front claws; then it
    // creeps forward a body length and stops again. That is the whole animal:
    // something you notice has moved rather than something you watch moving.
    //
    // Now and then - a minute or two apart - it does pick up and swim to
    // another part of the bottom, level and without the tail sweep a fish has.
    // That is the only time it is obvious, and it is over in a few seconds.
    const float sandY = f.cfg->yHi - 2;
    switch (f.act) {
      case ACT_PICK:                 // stopped, picking the spot over
        // it does not hold perfectly still; it shuffles and turns on the spot
        f.tx = f.x + (fcos(f.heading) >= 0 ? 14.0f : -14.0f);
        f.ty = f.holdY + fsin(sim.tw * 1.7f + f.phase) * 0.8f;
        want = 0.02f;
        f.actT -= dt; f.nextAct -= dt;
        if (f.actT <= 0) {
          if (f.nextAct <= 0) {
            f.act = ACT_SWIMOFF; f.actT = rnd(2.5f, 5.0f);
            f.holdX = clampf(f.x + rnd(-80, 80), 24, 296);
            f.holdY = rnd(f.cfg->yLo + 2, sandY);
          } else {
            // a body length, no more - but walked rather than oozed, because
            // something that only moves between glances reads as a glitch
            f.act = ACT_CRAWL; f.actT = rnd(2.0f, 5.0f);
            f.holdX = clampf(f.x + rnd(-22, 22), 20, 300);
            f.holdY = clampf(f.holdY + rnd(-4, 4), f.cfg->yLo, sandY);
          }
        }
        break;

      case ACT_CRAWL:                // a walk to the next patch
        f.tx = f.holdX; f.ty = f.holdY;
        want = 0.32f;
        f.actT -= dt; f.nextAct -= dt;
        if ((fabsf(f.x - f.holdX) < 5 && fabsf(f.y - f.holdY) < 4) || f.actT <= 0) {
          f.act = ACT_PICK; f.actT = rnd(5.0f, 16.0f);
        }
        break;

      case ACT_SWIMOFF:              // pleopods: level, unhurried, purposeful
        f.tx = f.holdX; f.ty = f.holdY;
        want = 1.1f;
        f.actT -= dt;
        if ((fabsf(f.x - f.holdX) < 10 && fabsf(f.y - f.holdY) < 6) || f.actT <= 0) {
          f.act = ACT_PICK; f.actT = rnd(6.0f, 14.0f);
          f.nextAct = rnd(55, 150);
        }
        break;

      case ACT_FLICK:                // the caridoid escape, still travelling
        // The burst that carries it is applied once, when the flick starts;
        // all this does is keep the body writhing and stop it steering while
        // it is going backwards.
        f.tx = f.x + fcos(f.heading) * 20.0f;
        f.ty = f.y + fsin(f.heading) * 20.0f;
        want = 0.0f;
        wantThrash = 1.0f;
        f.actT -= dt;
        if (f.actT <= 0) {
          f.act = ACT_PICK; f.actT = rnd(1.0f, 3.0f);
          f.holdY = clampf(f.y, f.cfg->yLo, sandY);
        }
        break;

      default:
        f.act = ACT_PICK; f.actT = rnd(2.0f, 6.0f);
        f.holdY = clampf(f.y, f.cfg->yLo, sandY);
        break;
    }

    // Startle. A prawn does not turn and swim away, it snaps its abdomen under
    // itself and shoots backwards - the caridoid escape reaction - covering
    // several body lengths in a few tenths of a second before it has any idea
    // where it is going. The burst decays on its own, so it is set once here.
    //
    // Rare, though. A flat per-frame probability is not a probability at all:
    // at sixteen frames a second it fired within one frame of every startle
    // and then again the next, and the sand ended up popping like corn. The
    // chance is per second now, and each shrimp then ignores everything for a
    // minute or so afterwards - which is also what the animal does.
    //
    // `retarget` is the open-water wander timer and no act ever reads it, so
    // for a shrimp it is free to hold the cooldown.
    if (f.retarget > 0) f.retarget -= dt;
    if (sim.stress > 0.55f && f.act != ACT_FLICK && f.retarget <= 0
        && rnd01() < dt * 0.55f) {
      f.act = ACT_FLICK;
      f.actT = rnd(0.30f, 0.55f);
      f.retarget = rnd(40, 120);
      f.burstX = -fcos(f.heading) * 175.0f;
      f.burstY = -fsin(f.heading) * 175.0f - 38.0f;   // backwards and up
    }
    // however far the flick threw it, it belongs back down on the sand
    if (f.act == ACT_PICK && f.y < f.cfg->yLo - 10) {
      f.act = ACT_SWIMOFF; f.actT = rnd(2.5f, 5.0f);
      f.holdX = clampf(f.x + rnd(-30, 30), 24, 296);
      f.holdY = rnd(f.cfg->yLo + 4, sandY);
    }

    f.thrash += (wantThrash - f.thrash)
              * fminf(1.0f, dt * (wantThrash > f.thrash ? 14.0f : 4.0f));
    float ks = (want > f.effort) ? fminf(1.0f, dt * 5.0f) : fminf(1.0f, dt * 2.2f);
    f.effort += (want - f.effort) * ks;
    return f.effort;
  }

  if (f.cfg->key == SP_CORY) {
    // Bottom work in short scoots with long pauses, broken by a bolt to the
    // surface for a gulp of air and a quiet glide back down. baseSpeed is the
    // dash speed; the crawl is the same scale at a very low effort.
    const float floorY = f.cfg->yHi - 1;
    const float skyY   = VIEW::y0 + 3;
    switch (f.act) {
      case ACT_GRAZE:                      // a short scoot along the substrate
        f.tx = f.holdX; f.ty = f.holdY;
        want = 0.22f;
        f.actT -= dt; f.nextAct -= dt;
        if ((fabsf(f.x - f.holdX) < 9 && fabsf(f.y - f.holdY) < 6) || f.actT <= 0) {
          f.act = ACT_REST; f.actT = rnd(2.5f, 8.0f);
          f.holdY = floorY - rnd(0, (int)(f.cfg->yHi - f.cfg->yLo) / 3);
        }
        break;

      case ACT_REST:                       // sat on the sand, barely a fin
        f.tx = f.x + (fcos(f.heading) >= 0 ? 20.0f : -20.0f);
        f.ty = f.holdY;
        want = 0.03f;
        f.actT -= dt; f.nextAct -= dt;
        if (f.actT <= 0) {
          if (f.nextAct <= 0) {
            f.act = ACT_DASH; f.actT = 5.0f;
            f.holdX = clampf(f.x + rnd(-45, 45), 40, 280);
          } else {
            f.act = ACT_GRAZE; f.actT = rnd(2.0f, 5.5f);
            f.holdX = clampf(f.x + rnd(-80, 80), 20, 300);
            f.holdY = floorY - rnd(0, (int)(f.cfg->yHi - f.cfg->yLo) / 3);
          }
        }
        break;

      case ACT_DASH:                       // straight up, whole body writhing
        f.tx = f.holdX; f.ty = skyY;
        want = 5.5f;
        wantThrash = 1.0f;
        f.actT -= dt;
        if (f.y < skyY + 6 || f.actT <= 0) { f.act = ACT_AIR; f.actT = rnd(0.2f, 0.45f); }
        break;

      case ACT_AIR:                        // the gulp itself is very brief
        f.tx = f.x + (fcos(f.heading) >= 0 ? 6.0f : -6.0f);
        f.ty = skyY - 2;
        want = 0.05f;
        f.actT -= dt;
        if (f.actT <= 0) {
          f.act = ACT_SETTLE; f.actT = 14.0f;
          f.holdX = clampf(f.x + rnd(-95, 95), 20, 300);
          f.holdY = floorY - rnd(0, (int)(f.cfg->yHi - f.cfg->yLo) / 3);
        }
        break;

      case ACT_SETTLE:                     // and quietly back down, gliding
        f.tx = f.holdX; f.ty = f.holdY;
        want = 0.9f;
        f.actT -= dt;
        if (f.y > f.holdY - 9 || f.actT <= 0) {
          f.act = ACT_GRAZE; f.actT = rnd(2.0f, 5.5f);
          f.nextAct = rnd(35, 130);
        }
        break;

      default:                             // never mid-water; back to the sand
        f.act = ACT_GRAZE; f.actT = rnd(1.5f, 4.0f);
        f.holdX = clampf(f.x + rnd(-70, 70), 20, 300);
        f.holdY = floorY - rnd(0, (int)(f.cfg->yHi - f.cfg->yLo) / 3);
        break;
    }
    // A startle makes it scurry, but only if it is already down on the sand.
    // Interrupting a descent used to drop it into ACT_REST in mid-water, where
    // the effort is near zero - it just hung there as if snagged on something.
    if (sim.stress > 0.45f && f.act == ACT_REST) {
      f.act = ACT_GRAZE; f.actT = rnd(1.0f, 2.5f);
      f.holdX = clampf(f.x + rnd(-80, 80), 20, 300);
    }
    // safety net for any other way it could end up off the bottom while it
    // thinks it is working the substrate: glide back down instead of hovering
    if ((f.act == ACT_GRAZE || f.act == ACT_REST) && f.y < f.cfg->yLo - 12) {
      f.act = ACT_SETTLE; f.actT = 14.0f;
      f.holdX = clampf(f.x + rnd(-40, 40), 20, 300);
      f.holdY = floorY - rnd(0, 4);
    }
    f.thrash += (wantThrash - f.thrash)
              * fminf(1.0f, dt * (wantThrash > f.thrash ? 9.0f : 3.0f));
    float ke = (want > f.effort) ? fminf(1.0f, dt * 6.0f) : fminf(1.0f, dt * 1.8f);
    f.effort += (want - f.effort) * ke;
    return f.effort;
  }

  switch (f.act) {
    case ACT_HOLD:
      // hanging in open water, fins ticking over, going nowhere
      f.tx = f.holdX + (fcos(f.heading) >= 0 ? 22.0f : -22.0f);
      f.ty = f.holdY + fsin(sim.tw * 0.7f + f.phase) * 2.0f;
      want = 0.05f;
      f.actT -= dt;
      if (f.actT <= 0) { f.act = ACT_SWIM; f.nextAct = rnd(1.5f, 5.0f); }
      break;

    case ACT_SWIM:
      f.nextAct -= dt;
      if (f.pers != PERS_NONE && f.nextAct <= 0 && sim.stress < 0.2f) {
        if (f.pers == PERS_HOVERER) {
          f.act = ACT_HOLD; f.actT = rnd(4, 15);
          f.holdX = f.x; f.holdY = f.y;
        } else if (f.pers == PERS_GULPER) {
          f.act = ACT_RISE; f.actT = 9.0f;
          f.holdX = clampf(f.x + rnd(-40, 40), 35, 285);
          f.holdY = surfaceY;
        } else {
          f.act = ACT_PARK; f.actT = 10.0f;
          const float lo = f.cfg->yLo, hi = f.cfg->yHi;
          if (rnd01() < 0.55f) {            // against a side pane
            f.holdX = (rnd01() < 0.5f) ? VIEW::x0 + 11 : VIEW::x1 - 11;
            f.holdY = rnd(lo + (hi - lo) * 0.3f, hi);
          } else {                          // hanging at the foot of its layer
            f.holdX = rnd(45, 275);
            f.holdY = hi - rnd(0, 8);
          }
        }
      }
      break;

    case ACT_RISE:
      f.tx = f.holdX; f.ty = f.holdY; want = 0.75f;
      f.actT -= dt;
      if (f.y < surfaceY + 6 || f.actT <= 0) { f.act = ACT_GULP; f.actT = rnd(0.7f, 1.7f); }
      break;

    case ACT_GULP:
      // hold station at the surface, nose up, barely moving
      f.tx = f.x + (fcos(f.heading) >= 0 ? 7.0f : -7.0f);
      f.ty = surfaceY - 2;
      want = 0.06f;
      f.actT -= dt;
      if (f.actT <= 0) {
        f.act = ACT_SINK; f.actT = 7.0f;
        f.holdY = rnd(VIEW::ymap(45), VIEW::ymap(92));
      }
      break;

    case ACT_SINK:
      f.tx = clampf(f.x + (fcos(f.heading) >= 0 ? 30.0f : -30.0f), 35, 285);
      f.ty = f.holdY; want = 0.5f;
      f.actT -= dt;
      if (fabsf(f.y - f.holdY) < 8 || f.actT <= 0) { f.act = ACT_SWIM; f.nextAct = rnd(12, 32); }
      break;

    case ACT_PARK:
      f.tx = f.holdX; f.ty = f.holdY; want = 0.7f;
      f.actT -= dt;
      if ((fabsf(f.x - f.holdX) < 10 && fabsf(f.y - f.holdY) < 8) || f.actT <= 0) {
        f.act = ACT_HOVER; f.actT = rnd(4, 13);
      }
      break;

    case ACT_HOVER:
      // sitting still, pointing along the pane, fins just ticking over
      f.tx = f.holdX + (fcos(f.heading) >= 0 ? 22.0f : -22.0f);
      f.ty = f.holdY + fsin(sim.tw * 0.9f + f.phase) * 1.5f;
      want = 0.045f;
      f.actT -= dt;
      if (f.actT <= 0) { f.act = ACT_SWIM; f.nextAct = rnd(15, 38); }
      break;
  }

  f.thrash += (0.0f - f.thrash) * fminf(1.0f, dt * 3.0f);
  float k = (want > f.effort) ? fminf(1.0f, dt * 6.0f) : fminf(1.0f, dt * 1.6f);
  f.effort += (want - f.effort) * k;
  return f.effort;
}

// --- behaviour -------------------------------------------------------------
static void stepFish(Sim& sim, Fish& f, float dt) {
  const SpeciesCfg* c = f.cfg;

  const float eff = stepAct(sim, f, dt);
  const bool busy = (f.act != ACT_SWIM);      // an act owns the steering target

  // steering target
  if (busy) {
    // stepAct() already set tx/ty
  } else if (f.school) {
    const School& s = sim.school[f.sid];
    float oa = sim.tw * 0.35f + f.phase;
    f.tx = s.x + fcos(oa) * f.orbitR;
    f.ty = s.y + fsin(oa * 0.8f + f.phase) * f.orbitR * 0.60f;
  } else {
    f.retarget -= dt;
    float dx = f.tx - f.x, dy = f.ty - f.y;
    if (f.retarget <= 0 || dx * dx + dy * dy < 100) {
      f.retarget = rnd(6, 16);
      if (c->roam > 0) {
        // holds a patch of the tank rather than crossing it
        f.tx = clampf(f.x + rnd(-c->roam, c->roam), 22, 298);
        f.ty = clampf(f.y + rnd(-c->roam * 0.45f, c->roam * 0.45f),
                      c->yLo, c->yHi);
      } else {
        // Aim at the far side of the tank rather than at a fresh uniform
        // point. A slow fish never reaches a uniform target before the timer
        // re-rolls it, and the mean of those targets is the middle of the
        // screen - which is exactly where every fish ends up. Picking a
        // destination it has to travel to keeps the tank evenly used.
        float lo, hi;
        if ((f.x < 160.0f) != (rnd01() < 0.25f)) {      // usually cross over
          lo = fminf(f.x + 70.0f, 250.0f); hi = 298.0f;
        } else {
          lo = 22.0f; hi = fmaxf(f.x - 70.0f, 70.0f);
        }
        f.tx = rnd(lo, hi);
        // upper part of its own layer most of the time, with occasional dives
        float mid = c->yLo + (c->yHi - c->yLo) * 0.55f;
        f.ty = rnd01() < 0.80f ? rnd(c->yLo, mid) : rnd(mid, c->yHi);
      }
    }
  }

  // soft wall avoidance - suspended while an act is deliberately holding the
  // fish against a pane, the substrate or the surface
  if (!busy) {
    if (f.x < VIEW::x0 + 8) f.tx = fmaxf(f.tx, 120.0f);
    if (f.x > VIEW::x1 - 8) f.tx = fminf(f.tx, 200.0f);
    if (f.y < c->yLo + 4) f.ty = fmaxf(f.ty, c->yLo + (c->yHi - c->yLo) * 0.35f);
    if (f.y > c->yHi - 4) f.ty = fminf(f.ty, c->yHi - (c->yHi - c->yLo) * 0.35f);
  }

  float desired = atan2f(f.ty - f.y, f.tx - f.x);
  float diff = wrapAng(desired - f.heading);
  float maxTurn = c->turnRate * dt * (0.75f + 0.5f * f.speedNorm);
  f.heading = fwrap(f.heading + clampf(diff, -maxTurn, maxTurn));

  float inst = wrapAng(f.heading - f.prevHeading) / fmaxf(dt, 1e-4f);
  f.prevHeading = f.heading;
  f.turnRate += (inst - f.turnRate) * fminf(1.0f, dt * 7);

  float spd = c->baseSpeed * (0.8f + 0.35f * fsin(sim.tw * 0.23f + f.phase));
  f.speed += (spd - f.speed) * fminf(1.0f, dt * 2);
  float boost = (1 + sim.stress * 1.3f
              + (f.school && !busy ? sim.school[f.sid].dash * 0.9f : 0.0f)) * eff;
  f.speedNorm = clampf((f.speed * boost) / (c->baseSpeed * 2.4f), 0, 1);

  // --- caught in the air stone ---------------------------------------------
  // Water moving is water moving; what differs between fish is how much of it
  // they go along with. A 3cm neon has almost no mass to anchor it and almost
  // no muscle to argue with, and both of those scale with its length, so the
  // ride it gets goes as roughly the square of how much smaller it is than the
  // reference. In the tank that reads clearly: neons are thrown up the column,
  // guppies drift up it, and a 5cm black tetra tips a little and swims on.
  float wx, wy;
  airFlowAt(f.x, f.y, &wx, &wy);
  if (wy != 0.0f) {
    const float r = LIFT_REF_CM / c->lenCm;
    const float k = clampf(r * r, 0.25f, 1.4f);
    wx *= k;
    wy *= k;
    // it does not take it lying down
    f.thrash = fmaxf(f.thrash, k * fminf(1.0f, -wy * (1.0f / 45.0f)) * 0.45f);
    f.lifted = 1.2f;
  }

  f.x += (fcos(f.heading) * f.speed * boost + f.burstX + f.sepX + wx) * dt;
  f.y += (fsin(f.heading) * f.speed * boost + f.burstY + f.sepY + wy) * dt;
  f.x = clampf(f.x, VIEW::x0 - 6, VIEW::x1 + 6);

  // The layer clamp would undo the lift the moment it applied, so it is held
  // open while the fish is in the plume and until it has swum back down - a
  // fixed timeout would snap a corydoras from mid-water back onto the sand.
  if (f.lifted > 0) {
    f.lifted -= dt;
    if (f.lifted <= 0 && f.y < c->yLo - 8) f.lifted = 0.4f;
  }
  // Free to leave the layer while an act is running (a guppy going up for air,
  // a corydoras bolting to the surface), but not otherwise. The shrimp is the
  // exception: every one of its acts is an act, so going by `busy` would leave
  // it unclamped for its whole life - and it lives in a strip of sand a dozen
  // rows deep, below which is the near edge of the photograph.
  const bool loose = (c->key == SP_SHRIMP)
                   ? (f.lifted > 0 || f.act == ACT_FLICK)
                   : (busy || f.lifted > 0);
  if (loose) f.y = clampf(f.y, VIEW::y0 - 6, VIEW::y1 + 4);
  else       f.y = clampf(f.y, c->yLo - 8, c->yHi + 8);

  f.burstX *= expf(-3 * dt);
  f.burstY *= expf(-3 * dt);

  f.beat += dt * (float)M_PI * 2 * c->beatHz *
            (0.65f + 0.9f * f.speedNorm + sim.stress * 0.5f + f.thrash * 2.2f);
  if (f.beat > TRIG_WRAP) f.beat -= TRIG_WRAP;

  stepDepth(f, dt);

  // The art is drawn head-right. Rotating it to a leftward heading would turn
  // the fish belly-up, so a leftward fish is mirrored instead. The threshold
  // keeps a fish that is swimming almost straight up or down from flapping
  // between the two.
  float chd = fcos(f.heading);
  if (chd > 0.15f) f.mirror = 1.0f;
  else if (chd < -0.15f) f.mirror = -1.0f;

  // The card does not mirror, it turns over - and it does not turn over as a
  // separate event either.
  //
  // Animating the flip on its own timer was the mistake. However carefully it
  // was paced, it was still a second thing happening beside the swimming, and
  // it always looked like the card stopped, turned, and then set off again.
  //
  // The card's face simply follows the direction it is travelling: broadside
  // while it is crossing the tank, edge-on when it is pointed straight up or
  // down, and over onto its back once it has come about. Now there is only one
  // motion. It cannot finish rotating before it starts moving, because the
  // rotation *is* the movement - the card is only ever part-way over because
  // the fish is part-way round its arc.
  //
  // The page-like pacing comes free with it. Width is cos(heading), so its
  // rate of change is sin(heading) x turn rate: barely moving while the card
  // is near flat, fastest as it passes through edge-on, settling again on the
  // other side. That is the profile the hand-written easing was trying to
  // imitate, and this one cannot drift out of step with the fish.
  if (c->key == SP_CARD) {
    // signed width, flattened so it only goes truly thin near the reversal
    const float w = (chd >= 0 ? 1.0f : -1.0f)
                  * powf(fabsf(chd), CARD_FACE_FLAT);
    const float target = acosf(clampf(w, -1.0f, 1.0f));
    f.turn += (target - f.turn) * fminf(1.0f, dt * CARD_FACE_EASE);
  }

  // Fin billow. A guppy's fan barely moves while it cruises; it opens like a
  // skirt when the fish banks into a turn and then settles back slowly, so the
  // envelope attacks fast and releases slow.
  DepthEv& d = f.depth;
  {
    float want = clampf(d.bell * 0.9f + fabsf(f.turnRate) * 0.45f, 0, 1.5f);
    float k = (want > f.flare) ? fminf(1.0f, dt * 10.0f) : fminf(1.0f, dt * 2.2f);
    f.flare += (want - f.flare) * k;
  }

  float roll = clampf(f.turnRate * 0.35f + d.bell * d.sign * 1.1f, -2, 2);
  f.biasF += (roll - f.biasF) * fminf(1.0f, dt * 9);
  f.facing = 1 - d.bell * 0.85f;

  stepChain(f);
}

static void stepSchool(School& s, float dt) {
  s.timer -= dt;
  float dx = s.tx - s.x, dy = s.ty - s.y;
  float dist = sqrtf(dx * dx + dy * dy);
  if (s.timer <= 0 || dist < 12) {
    s.timer = rnd(5, 11);
    bool far = rnd01() < 0.25f;
    s.dash = far ? 1.0f : 0.0f;
    // each shoal roams around its own stretch of the tank
    s.tx = clampf(s.home + rnd(-95, 95) + (far ? rnd(-60, 60) : 0), 25, 295);
    s.ty = clampf(s.homeY + rnd(-20, 20), NEON.yLo + 8, NEON.yHi - 8);
  }
  float v = s.dash > 0 ? 60.0f : 22.0f;
  if (dist > 0.5f) {
    s.x += (dx / dist) * fminf(v * dt, dist);
    s.y += (dy / dist) * fminf(v * dt, dist);
  }
  s.dash = fmaxf(0.0f, s.dash - dt * 0.5f);
}

// Crowd separation. The browser build has none - with 8 tetras on one orbit it
// never needed it, but 20 of them stack into a single blob. Neighbours inside
// roughly one body length push each other apart, vertically a little less so
// the school keeps its flat shape.
static void stepSeparation(Sim& sim) {
  const float STRENGTH = 26.0f;      // px/s at contact
  for (int i = 0; i < sim.n; i++) { sim.fish[i].sepX = 0; sim.fish[i].sepY = 0; }
  for (int i = 0; i < sim.n; i++) {
    Fish& a = sim.fish[i];
    for (int j = i + 1; j < sim.n; j++) {
      Fish& b = sim.fish[j];
      float dx = b.x - a.x, dy = b.y - a.y;
      float R = (a.cfg->W + b.cfg->W) * 0.45f;
      float d2 = dx * dx + dy * dy;
      if (d2 >= R * R || d2 < 1e-4f) continue;
      float d = sqrtf(d2);
      float push = (1 - d / R) * STRENGTH;
      float ux = dx / d, uy = dy / d;
      a.sepX -= ux * push;  a.sepY -= uy * push * 0.7f;
      b.sepX += ux * push;  b.sepY += uy * push * 0.7f;
    }
  }
}

// ---------------------------------------------------------------------------
// A corydoras blunders into a shrimp.
//
// The Amano are the hardest thing in the tank to notice: small, translucent,
// the colour of the sand, and deliberately almost motionless. They are also
// sharing the substrate with two catfish that spend all day working along it
// with their heads down. In a real tank that meeting happens constantly and it
// always ends the same way - the shrimp is gone before the fish has registered
// it was there, which is the single most shrimp-like thing an Amano does and,
// conveniently, the thing most likely to make you look at one.
//
// So it is not a startle, it is a contact reflex: no threshold on `stress`, no
// minute-long cooldown, no dice roll. Close enough, and it goes.
static const float CORY_TOUCH = 18.0f;    // px between centres

static void stepShrimpScatter(Sim& sim) {
  for (int i = 0; i < sim.n; i++) {
    Fish& sh = sim.fish[i];
    if (sh.cfg->key != SP_SHRIMP || sh.act == ACT_FLICK) continue;
    for (int j = 0; j < sim.n; j++) {
      const Fish& co = sim.fish[j];
      if (co.cfg->key != SP_CORY) continue;
      const float dx = sh.x - co.x, dy = sh.y - co.y;
      const float d2 = dx * dx + dy * dy;
      if (d2 > CORY_TOUCH * CORY_TOUCH) continue;

      // Away from the fish rather than simply backwards. A real prawn does not
      // aim - it snaps its abdomen and goes wherever that sends it - but at
      // 320x240 a shrimp that flicks *into* the catfish reads as a collision
      // rather than as an escape, and the whole point of this is to be read.
      const float d = sqrtf(d2);
      float ux, uy;
      if (d > 0.01f) { ux = dx / d; uy = dy / d; }
      else           { ux = (rnd01() < 0.5f ? -1.0f : 1.0f); uy = 0.0f; }
      // and off the bottom, because that is where the room is
      uy -= 0.85f;
      const float n = sqrtf(ux * ux + uy * uy);

      sh.act = ACT_FLICK;
      sh.actT = rnd(0.28f, 0.48f);
      sh.burstX = ux / n * 200.0f;
      sh.burstY = uy / n * 200.0f;
      sh.thrash = 1.0f;
      // Do not touch `retarget`: that is the cooldown on being startled by the
      // tank at large, and being trodden on is not the same thing. It is the
      // flick's own half second, plus the sixty-odd pixels the burst carries
      // it, that stops this re-triggering every frame.
      break;
    }
  }
}

void stepSim(Sim& sim, float dt) {
  sim.t += dt;
  sim.tw += dt;
  if (sim.tw > TRIG_WRAP) sim.tw -= TRIG_WRAP;
  sim.stress *= expf(-1.1f * dt);
  for (int k = 0; k < N_SCHOOLS; k++) stepSchool(sim.school[k], dt);
  stepSeparation(sim);
  stepShrimpScatter(sim);
  for (int i = 0; i < sim.n; i++) stepFish(sim, sim.fish[i], dt);

  // water sway: damped spring driven by tap surges
  for (int i = sim.nSurge - 1; i >= 0; i--) {
    Surge& s = sim.surges[i];
    s.t += dt;
    if (s.t < s.dur) {
      float p = s.t / s.dur;
      sim.swayV += s.dir * s.mag * fsin(p * (float)M_PI) * dt * 8;
    } else {
      sim.surges[i] = sim.surges[--sim.nSurge];
    }
  }
  sim.swayV += -sim.sway * 26 * dt - sim.swayV * 5.5f * dt;
  sim.sway += sim.swayV * dt;
  sim.sway = clampf(sim.sway, -3.5f, 3.5f);

  // bubbles
  sim.nextBubble -= dt;
  if (sim.nextBubble <= 0 && sim.nBub < MAX_BUB) {
    sim.nextBubble = rnd(1.6f, 4.5f);
    sim.bubbles[sim.nBub++] = { rnd(20, 300), VIEW::floorY - 3.0f,
                                rnd01() < 0.7f ? 0.8f : 1.3f,
                                rnd(9, 16), rnd(0, 6), rnd(0.25f, 0.5f) };
  }
  for (int i = sim.nBub - 1; i >= 0; i--) {
    Bubble& b = sim.bubbles[i];
    b.y -= b.vy * dt;
    b.x += fsin(sim.tw * 3 + b.ph) * 3 * dt;
    if (b.y < VIEW::horizonY + 4) sim.bubbles[i] = sim.bubbles[--sim.nBub];
  }

  // drifting motes
  for (int i = 0; i < N_MOTES; i++) {
    Mote& m = sim.motes[i];
    m.x += (m.vx + fsin(sim.tw * 0.4f + m.ph) * 0.5f) * dt;
    m.y += m.vy * dt;
    if (m.y > VIEW::floorY - 3) m.y = VIEW::horizonY + 3;
    if (m.y < VIEW::horizonY + 2) m.y = VIEW::floorY - 4;
    if (m.x < 6) m.x = 314;
    if (m.x > 314) m.x = 6;
  }
}

// The water column sloshes sideways, the school darts away, nearby fish burst
// and a couple lunge toward the viewer. No surface ripples (side view).
void tapWater(Sim& sim, float x, float y) {
  float dir = x < 160 ? 1.0f : -1.0f;
  if (sim.nSurge < MAX_SURGE) sim.surges[sim.nSurge++] = { 0, 0.9f, dir, 1 };
  sim.stress = fminf(1.0f, sim.stress + 0.7f);
  // every shoal darts away from the tap
  for (int k = 0; k < N_SCHOOLS; k++) {
    School& s = sim.school[k];
    // bolting away from the tap, but still around its own stretch of glass -
    // otherwise a tap near one end herds every shoal into the same corner
    s.tx = clampf(s.home + (s.home < x ? -1.0f : 1.0f) * rnd(35, 95), 25, 295);
    s.ty = clampf(s.homeY + rnd(-22, 22), NEON.yLo + 8, NEON.yHi - 8);
    s.timer = 4; s.dash = 1;
  }

  // nearby fish burst away; the two closest lunge toward the viewer
  int   order[N_FISH];
  float dist[N_FISH];
  for (int i = 0; i < sim.n; i++) {
    order[i] = i;
    float dx = sim.fish[i].x - x, dy = sim.fish[i].y - y;
    dist[i] = sqrtf(dx * dx + dy * dy);
  }
  for (int i = 1; i < sim.n; i++) {          // insertion sort by distance
    int   ki = order[i];
    float kd = dist[ki];
    int   j = i - 1;
    while (j >= 0 && dist[order[j]] > kd) { order[j + 1] = order[j]; j--; }
    order[j + 1] = ki;
  }
  for (int k = 0; k < sim.n; k++) {
    Fish& f = sim.fish[order[k]];
    float d = dist[order[k]];
    if (d > 110) break;
    float kk = 1 - d / 110;
    float ang = atan2f(f.y - y, f.x - x);
    f.burstX += fcos(ang) * 85 * kk;
    f.burstY += fsin(ang) * 60 * kk;
  }
  for (int i = 0; i < 2 && i < sim.n; i++) {
    if (dist[order[i]] < 130) startDepthEvent(sim.fish[order[i]], true);
  }
}
