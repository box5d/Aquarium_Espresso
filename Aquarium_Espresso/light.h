#ifndef PIXAQ_LIGHT_H
#define PIXAQ_LIGHT_H
// ---------------------------------------------------------------------------
// light.h — tank lighting: LED inverter flicker + the caustic net the surface
// throws onto everything below it.
//
// The backdrop photo is a still image, so all of the life in the tank has to
// come from how it is lit. Three things are layered onto it every frame:
//
//   flicker    a ripple on the LED bar's supply. Real fixtures ripple at
//              100/120Hz; on a ~30fps panel that aliases into a slow shimmer,
//              so what is modelled here is the beat, not the raw ripple, plus
//              the occasional deeper dip a tired driver makes.
//   caustics   three travelling wave trains read from one sharpened profile
//              LUT, plus a fourth that is much broader and smoother. Where the
//              sharp three cross, the bright lines pile up into cells - the
//              same interference a real surface throws - while the broad one
//              drifts a pool of brightness across the whole scene.
//
//              The point of three rather than two: two trains give a plaid
//              that slides about as a rigid sheet. Caustics do not do that.
//              Their forms persist while their positions shift, and the forms
//              themselves slowly re-make - lines wander, meet, split. So the
//              three trains are not fixed: their directions and wavelengths
//              drift continuously, which re-forms the pattern in place instead
//              of scrolling it past. That is the shimmer; nothing in the scene
//              is ever moved or distorted to get it.
//
// Cost matters: this touches all 76800 pixels every frame, so the waves are
// LUT-driven with a fixed-point accumulator per pixel and the colour scaling
// is integer-only.
// ---------------------------------------------------------------------------
#include <Arduino.h>

void lightInit();

// Brighten the decoded backdrop once, before any of the above runs. The
// runtime lighting only ever darkens (that is what lets the pixel loop skip
// its clamp), so without a lift the tank sits at about 80% brightness all the
// time and reads as a dim tank rather than a shop display.
void lightPrepBackdrop();
void lightStep(float dt);
void lightApply(int y0, int y1);   // BGBUF -> FB, lit, rows [y0,y1)

// The same lighting the backdrop gets, sampled at one point. The fish are
// composited after the backdrop, so without this they sit at full brightness
// on top of a shaded tank and read as stickers rather than as something in
// the water with the light moving over it. 8.8 fixed, 256 = unity.
int lightGainAt(int x, int y);

#endif // PIXAQ_LIGHT_H
