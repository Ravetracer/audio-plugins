#pragma once

#include <cmath>
#include <cstdint>

namespace plugincore {

// Clamp without pulling in <algorithm> in the audio path.
template <typename T> inline T clampv(T v, T lo, T hi) {
   return v < lo ? lo : (v > hi ? hi : v);
}

// sin(2*pi*t) for arbitrary t. Quarter-wave reduction plus a 9th order Taylor
// series, which is accurate to ~1e-8 over [0, pi/2] and costs a handful of
// multiplies -- cheap enough to run per voice, per sample.
inline float sin2pi(float t) {
   t -= std::floor(t);
   float sign = 1.0f;
   if (t >= 0.5f) {
      t -= 0.5f;
      sign = -1.0f;
   }
   if (t > 0.25f)
      t = 0.5f - t;
   const float x = t * 6.283185307179586f;
   const float x2 = x * x;
   const float y =
      x * (1.0f + x2 * (-1.0f / 6.0f +
                        x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
   return sign * y;
}

// The same function from a table, for inner loops where it is evaluated
// several times per voice per sample. 4096 entries with linear
// interpolation: the largest error is (2 pi / 4096)^2 / 8, about 3e-7, which is
// -130 dB and below anything a 24-bit converter can carry. Measured against the
// series it saves only a few per cent -- such loops are usually bound by their
// chain of filters, not by arithmetic -- but it is exact enough to be free.
struct SinTable {
   static constexpr int kSize = 4096;
   float v[kSize + 1];
   SinTable() {
      for (int i = 0; i <= kSize; ++i)
         v[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * i / kSize));
   }
};
inline const SinTable &sinTable() {
   static const SinTable table;
   return table;
}

inline float sin2piFast(float t) {
   t -= std::floor(t);
   const float x = t * static_cast<float>(SinTable::kSize);
   const int i = static_cast<int>(x);
   const float f = x - static_cast<float>(i);
   const float *v = sinTable().v;
   return v[i] + f * (v[i + 1] - v[i]);
}

// Decay coefficient for an exponential envelope that falls to -60 dB after
// `seconds`. Multiplicative, one multiply per sample.
inline float decayCoef(float seconds, float sampleRate) {
   if (seconds <= 0.0f)
      return 0.0f;
   return std::exp(-6.907755279f / (seconds * sampleRate)); // ln(1000) = 6.9078
}

// One-pole coefficient for a filter approaching its target with the given time
// constant (63.2% of the way there after `seconds`).
inline float onePoleCoef(float seconds, float sampleRate) {
   if (seconds <= 0.0f)
      return 1.0f;
   return 1.0f - std::exp(-1.0f / (seconds * sampleRate));
}

inline float dbToGain(float db) { return db <= -59.9f ? 0.0f : std::pow(10.0f, db * 0.05f); }

inline float semitonesToRatio(float semis) { return std::exp2(semis * (1.0f / 12.0f)); }

// The suite's output stage. Linear to 0.8 and a tanh knee above it, so a
// plugin driven into its own ceiling saturates rather than clipping, and the
// output stays bounded whatever the gain staging inside it adds up to.
inline float softClip(float x) {
   constexpr float t = 0.8f;
   if (x > t)
      return t + (1.0f - t) * std::tanh((x - t) / (1.0f - t));
   if (x < -t)
      return -t - (1.0f - t) * std::tanh((-x - t) / (1.0f - t));
   return x;
}

} // namespace plugincore
