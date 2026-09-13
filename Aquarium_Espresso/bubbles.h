#ifndef PIXAQ_BUBBLES_H
#define PIXAQ_BUBBLES_H
// ---------------------------------------------------------------------------
// bubbles.h — the air pump.
//
// One air stone sits buried in the sand, towards one end of the tank or the
// other, at a position drawn at boot. It runs a fine plume of bubbles up to the
// surface, fanning out as it climbs. No stone is drawn: pasting a fake object
// onto a photograph is exactly what made the fish read as stickers, and a
// sand-bed diffuser is invisible anyway.
//
// The pump is not only decoration. Every bubble that reaches the top breaks the
// surface, and a broken surface is what the caustic net is a picture of - so
// the agitation this module accumulates is fed back into light.cpp, where it
// drives the whole net faster and harder. That is the difference between a tank
// with an air line in it and a tank without one.
// ---------------------------------------------------------------------------
#include <Arduino.h>

void bubblesInit();
void bubblesStep(float dt);
void bubblesDraw(int y0, int y1);   // rows [y0,y1) only, composited over the
                                    // lit backdrop and behind the fish

// How hard the pump is working the surface right now, 0 (dead still) to 1.
// Never settles: a diaphragm pump pulses, and the bubbles arrive in gusts.
float airAgitation();

// Water velocity at a point, px/s, from the column the plume drags up with it.
// This is the whole reason air stones are used as circulation in the first
// place: the bubbles themselves carry almost nothing, but each one drags a
// sleeve of water along, and a few thousand of them add up to a slow chimney
// that pulls water off the bottom and spills it out across the surface.
//
// Both components come back zero outside the plume. +y is down, as it is
// everywhere else in this build, so a lift arrives negative.
void airFlowAt(float x, float y, float* vx, float* vy);

#endif // PIXAQ_BUBBLES_H
