#pragma once

// A pocket of air trapped in water: the physics two plugins in the suite need,
// and a third will.
//
// Water folding over a stone, a wave collapsing, a drop landing on a wet
// surface -- all three trap air, and the trapped air rings. RainyDay's
// droplets, ShoreBreak's foam bubbles and RiverFlow's pockets are the same
// object arrived at three times, so the relations live here rather than in each
// of them.

#include <cmath>

#include "fastmath.h"

namespace plugincore {

// Minnaert's relation (1933), which is why a pocket of air has a pitch at all:
//
//   f0 = (1 / 2 pi r) sqrt(3 gamma p0 / rho) = 3.26 / r   for air in water at STP
//
// with r in millimetres and f0 in kHz. A 3.3 mm pocket rings at 988 Hz.
//
// It is worth trusting. RiverFlow's reference library measures a median dabble
// pitch of 984 Hz and, from the width of the same events' spectra, a median
// radius of 3.3 mm -- two independent measurements agreeing with the relation
// to one per cent. Its drops measure 2625 Hz against 1.24 mm, which is 2629 Hz
// predicted.
inline float minnaertHz(float radiusMm) { return 3260.0f / clampv(radiusMm, 0.05f, 200.0f); }

// The inverse, for a plugin whose parameter is a pitch rather than a size.
inline float minnaertRadiusMm(float hz) { return 3260.0f / clampv(hz, 16.0f, 40000.0f); }

// A pocket's damping, after Xue et al. (2023) eq. 3-5. Two mechanisms matter
// for anything audible:
//
//   delta_rad = omega0 r / c, and since Minnaert fixes r for a given f0 this is
//               2 pi * 3.26 / c = 0.01368 whatever the size -- radiative loss
//               is the same fraction for every pocket.
//   delta_th  is well approximated by 2/sqrt(psi) = 4.743e-4 sqrt(f0) over the
//               audible range.
//   delta_vis stays below 4e-4 for anything audible and is dropped.
//
// Q is 1/delta: about 20 for a small high pocket and 46 for a large low one,
// which puts ring times at 2-124 ms over 0.5-12 mm. Measured event envelopes
// fall 10 dB in 7-34 ms for a dabble and 8-86 ms for a drop, so the physics and
// the recordings agree without anything being fitted.
inline float bubbleDelta(float f) { return 0.01368f + 4.743e-4f * std::sqrt(f); }

// The amplitude decay rate of the oscillator, in nepers per second: a pocket's
// envelope is e^(-beta t).
inline float bubbleBeta(float f, float dampingMul = 1.0f) {
   const float delta = clampv(bubbleDelta(f) * dampingMul, 0.002f, 0.6f);
   return 3.14159265f * f * delta;
}

// What a surface does to the impact that struck it.
//
// This is the part that separates a drop landing on concrete from a drop
// landing in a bucket, and it is measured rather than a matter of taste. The
// event-triggered spectra of RiverFlow's references give an impact a Q of about
// 6 -- broad enough to have no pitch of its own and to take its colour from the
// surface. An impact modelled as a damped sine instead has whatever Q its
// damping gives it; RainyDay's measured 18, which is why its drops read as a
// tuned plink where the recordings read as a tack off stone.
//
// So: a short burst of noise through a broad resonance, not a tone.
constexpr float kImpactQ = 6.0f;

// Svf::setCutoff takes resonance as 0..1, mapped to k = 1/Q over 2..0.02.
// Passing a Q straight in silently clamps to maximum resonance, which turns a
// noise band into a whistle. This converts properly, and every plugin in the
// suite has made that mistake at least once.
inline float resonanceForQ(float q) {
   const float k = 1.0f / (q > 0.5f ? q : 0.5f);
   return clampv((2.0f - k) / 1.98f, 0.0f, 1.0f);
}

} // namespace plugincore
