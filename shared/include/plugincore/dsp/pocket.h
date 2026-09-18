#pragma once

// A drop landing on a surface, as a sounding voice.
//
// This is RiverFlow's Trickle layer, lifted out so that RainyDay can use the
// same one. Not a reimplementation of it: the same code, drawing its random
// numbers in the same order, so the two plugins produce the same sound from
// the same settings.
//
// What a drop landing on wet stone actually is, in three parts:
//
//   the pocket    A decaying sinusoid at the Minnaert pitch for the radius of
//                 air the drop entrained, bending upward as the pocket shrinks.
//                 Struck at phase zero, because that is where the impulse
//                 response of a damped oscillator starts and anywhere else is
//                 a click.
//   the water     A band of noise at the pocket's own pitch: the water being
//                 displaced around it. Without this a cascade of pockets is a
//                 music box.
//   the surface   One band, at the surface's own resonance, carrying both the
//                 impact and the wash that follows it -- the impact is the fast
//                 decay through that band and the splash is the slow one. This
//                 is the part that makes it stone rather than a bucket.
//
// Sizes are radii in millimetres, because that is what a pocket's pitch
// physically is; see bubble.h for Minnaert's relation and why it is trusted.

#include <cmath>

#include "bubble.h"
#include "fastmath.h"
#include "filters.h"
#include "rng.h"

namespace plugincore {

// One pocket of air, sounding. Pooled and walked every sample while alive, so
// it carries four bytes of RNG state rather than a full generator.
struct Pocket {
   bool active = false;
   // A cluster of pockets is spread over its spill time, so a pocket can be
   // spawned now and start later. Counted down before anything else happens.
   int delaySamples = 0;

   // The ringing air.
   float phase = 0.0f;
   float inc = 0.0f;
   float level = 0.0f;
   float decayCoef = 0.0f;
   // Expressed as the total rise spread over the pocket's life. Applying it as
   // a per-sample factor compounds it into the megahertz within a millisecond,
   // which is a bug this suite has already had twice.
   float chirp = 1.0f;

   // The water around it being displaced.
   Svf bodyBand;
   float noiseMix = 0.0f;

   // What it struck.
   Svf surfaceBand;
   float clickLevel = 0.0f;
   float clickCoef = 0.0f;
   float splashLevel = 0.0f;
   float splashCoef = 0.0f;

   float panL = 0.7071f, panR = 0.7071f;
   RngLite rng;

   inline bool finished() const {
      return delaySamples <= 0 && level < 1.0e-5f && clickLevel < 1.0e-6f &&
             splashLevel < 1.0e-6f;
   }
};

// Everything a drop needs to know about itself. Levels and rates stay with the
// caller; this is the shape of one event.
struct TrickleSpec {
   float sizeMm = 1.24f;      // radius of the pocket entrained, hence its pitch
   float spreadOct = 1.2f;    // spread of sizes, in octaves
   float decaySec = 0.013f;   // how long one drop lasts
   float impact = 0.45f;      // the strike against the pocket that follows it
   float stoneToneHz = 3500.0f; // the surface's own resonance
   float splash = 0.25f;      // the wash where the water is deeper
   float width = 0.55f;       // stereo spread of the population
   float levelSigma = 0.4f;   // log-normal spread of event levels, in octaves
   float smear = 1.0f;        // distance smearing of the edges, 1 = none
};

// A log-normal multiplier with unit mean, `sigmaOct` octaves wide, bounded at
// three sigma. Event energies follow the volume of water involved, which is
// log-normal rather than uniform -- drawing them uniformly costs about 4 dB of
// crest factor. Bounded because a real distribution is, and because an
// unbounded tail drives an output stage into its clipper.
inline float pocketLogNormal(Rng &rng, float sigmaOct) {
   const float g = clampv(rng.gaussian(), -3.0f, 3.0f);
   const float f = std::exp2(g * sigmaOct);
   const float s = sigmaOct * 0.6931472f;
   return f / std::exp(0.5f * s * s);
}

// Start one drop. `levelBase` is the peak amplitude before this event's own
// log-normal scatter is applied.
//
// The order of draws from `rng` is part of the contract: two plugins sharing
// this function render identically from the same seed only while it is
// unchanged, so do not reorder them.
inline void spawnTricklePocket(Pocket &p, const TrickleSpec &spec, Rng &rng, float sampleRate,
                               float levelBase) {
   p.active = true;
   p.rng.seed(rng.next() | 1u);
   p.delaySamples = 0;

   const float pan = clampv(rng.white() * clampv(spec.width, 0.0f, 1.0f), -1.0f, 1.0f);
   p.panL = std::sqrt(0.5f * (1.0f - pan));
   p.panR = std::sqrt(0.5f * (1.0f + pan));

   const float oct = rng.gaussian() * clampv(spec.spreadOct, 0.0f, 3.0f) * 0.5f;
   const float r = clampv(spec.sizeMm * std::exp2(oct), 0.05f, 12.0f);
   const float f = clampv(minnaertHz(r), 200.0f, 0.45f * sampleRate);

   // A drop's pocket is asked to last a set time rather than to obey the
   // physics alone: the decay is measured directly from event-triggered
   // envelopes, and no damping control is offered, because a drop that has just
   // landed is not a free bubble.
   const float decay = clampv(spec.decaySec, 0.001f, 0.5f) * spec.smear;
   p.decayCoef = decayCoef(decay * 0.5f, sampleRate);
   const float lifeSamples = std::max(4.0f, decay * 0.5f * sampleRate);

   p.phase = 0.0f;
   p.inc = f / sampleRate;
   const float rise = 1.04f + 0.12f * rng.uniform();
   p.chirp = std::pow(rise, 1.0f / lifeSamples);

   const float impact = clampv(spec.impact, 0.0f, 1.0f);
   const float lvl = levelBase * pocketLogNormal(rng, spec.levelSigma);
   p.level = lvl * (1.0f - 0.75f * impact);

   p.bodyBand.reset();
   p.bodyBand.setCutoff(clampv(f * 1.1f, 80.0f, 0.45f * sampleRate), resonanceForQ(2.4f),
                        sampleRate);
   p.noiseMix = 0.35f + 0.4f * rng.uniform();

   // What it landed on. Hard wet rock is high and short; moss and gravel are
   // lower and duller.
   const float stone =
      clampv(spec.stoneToneHz * (0.8f + 0.4f * rng.uniform()), 200.0f, 0.45f * sampleRate);
   p.surfaceBand.reset();
   p.surfaceBand.setCutoff(stone, resonanceForQ(1.1f), sampleRate);
   p.clickLevel = lvl * impact * 0.6f;
   p.clickCoef = decayCoef(0.0022f * spec.smear, sampleRate);

   p.splashLevel = lvl * clampv(spec.splash, 0.0f, 1.0f) * 0.9f;
   p.splashCoef = decayCoef(decay * 3.0f, sampleRate);
}

// Render a pool of pockets into a stereo block, additively.
inline void processPocketPool(Pocket *pool, size_t count, float *outL, float *outR,
                              uint32_t numSamples) {
   for (size_t k = 0; k < count; ++k) {
      Pocket &p = pool[k];
      if (!p.active)
         continue;
      for (uint32_t i = 0; i < numSamples; ++i) {
         if (p.delaySamples > 0) {
            --p.delaySamples;
            continue;
         }
         p.inc *= p.chirp;
         // A ceiling well short of Nyquist, whatever the chirp is asked for.
         // sin2piFast reads an interpolated 4096-entry table, and a phase
         // increment near 0.5 walks it in steps large enough for the
         // interpolation error to become broadband noise rather than a tone.
         if (p.inc > 0.40f)
            p.inc = 0.40f;
         p.phase += p.inc;
         if (p.phase >= 1.0f)
            p.phase -= 1.0f;

         float s = sin2piFast(p.phase) * p.level;
         if (p.noiseMix > 1.0e-4f)
            s += p.bodyBand.bandpassNormalised(p.rng.white()) * p.level * p.noiseMix;

         if (p.clickLevel > 1.0e-6f || p.splashLevel > 1.0e-6f) {
            const float surf = p.surfaceBand.bandpassNormalised(p.rng.white());
            s += surf * (p.clickLevel + p.splashLevel);
            p.clickLevel *= p.clickCoef;
            p.splashLevel *= p.splashCoef;
         }

         outL[i] += s * p.panL;
         outR[i] += s * p.panR;
         p.level *= p.decayCoef;
      }
      if (p.finished())
         p.active = false;
   }
}

} // namespace plugincore
