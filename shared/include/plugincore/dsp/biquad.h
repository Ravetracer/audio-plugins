#pragma once

#include <cmath>

#include "fastmath.h"

namespace plugincore {

// A direct-form-I biquad with the Audio EQ Cookbook's shelf and peak designs.
//
// The suite's other filters (Svf, OnePoleLp, Hp2, Lp2) are all corners: they
// decide where a signal stops. A shelf or a bell decides how loud a band is
// while leaving the rest alone, which is a different job and the one an EQ
// does. Nothing in the suite could do it before WhooshPact needed one.
//
// Direct form I on purpose. Its state is delayed input and delayed output --
// the signal itself -- rather than an internal quantity that only means
// something for one particular set of coefficients, so recomputing the
// coefficients while it is running does not step the output. These are EQ
// bands whose frequency and gain a user drags around, so that matters.
class Biquad {
public:
   void reset() { mX1 = mX2 = mY1 = mY2 = 0.0f; }

   // A flat response. Used when a band's gain is zero, so that an untouched
   // band costs nothing but the multiplies.
   void setBypass() {
      mB0 = 1.0f;
      mB1 = mB2 = mA1 = mA2 = 0.0f;
   }

   void setLowShelf(float freqHz, float gainDb, float sampleRate, float slope = 0.7f) {
      shelf(freqHz, gainDb, sampleRate, slope, true);
   }

   void setHighShelf(float freqHz, float gainDb, float sampleRate, float slope = 0.7f) {
      shelf(freqHz, gainDb, sampleRate, slope, false);
   }

   void setPeaking(float freqHz, float gainDb, float q, float sampleRate) {
      if (gainDb > -0.01f && gainDb < 0.01f) {
         setBypass();
         return;
      }
      const float A = std::pow(10.0f, gainDb * (1.0f / 40.0f));
      const float w = twoPiOverSr(freqHz, sampleRate);
      const float cs = std::cos(w), sn = std::sin(w);
      const float alpha = sn / (2.0f * (q > 0.05f ? q : 0.05f));
      const float a0 = 1.0f + alpha / A;
      normalise(1.0f + alpha * A, -2.0f * cs, 1.0f - alpha * A, -2.0f * cs, 1.0f - alpha / A, a0);
   }

   inline float tick(float in) {
      const float y = mB0 * in + mB1 * mX1 + mB2 * mX2 - mA1 * mY1 - mA2 * mY2;
      mX2 = mX1;
      mX1 = in;
      mY2 = mY1;
      mY1 = y;
      return y;
   }

private:
   static float twoPiOverSr(float freqHz, float sampleRate) {
      const float nyq = 0.49f * sampleRate;
      const float f = clampv(freqHz, 10.0f, nyq);
      return 6.28318530717958647692f * f / sampleRate;
   }

   void shelf(float freqHz, float gainDb, float sampleRate, float slope, bool low) {
      if (gainDb > -0.01f && gainDb < 0.01f) {
         setBypass();
         return;
      }
      const float A = std::pow(10.0f, gainDb * (1.0f / 40.0f));
      const float w = twoPiOverSr(freqHz, sampleRate);
      const float cs = std::cos(w), sn = std::sin(w);
      const float S = clampv(slope, 0.1f, 2.0f);
      const float beta = sn * std::sqrt((A * A + 1.0f) * (1.0f / S - 1.0f) + 2.0f * A);
      const float ap1 = A + 1.0f, am1 = A - 1.0f;
      if (low) {
         const float a0 = ap1 + am1 * cs + beta;
         normalise(A * (ap1 - am1 * cs + beta), 2.0f * A * (am1 - ap1 * cs),
                   A * (ap1 - am1 * cs - beta), -2.0f * (am1 + ap1 * cs),
                   ap1 + am1 * cs - beta, a0);
      } else {
         const float a0 = ap1 - am1 * cs + beta;
         normalise(A * (ap1 + am1 * cs + beta), -2.0f * A * (am1 + ap1 * cs),
                   A * (ap1 + am1 * cs - beta), 2.0f * (am1 - ap1 * cs),
                   ap1 - am1 * cs - beta, a0);
      }
   }

   void normalise(float b0, float b1, float b2, float a1, float a2, float a0) {
      if (a0 > -1e-12f && a0 < 1e-12f) {
         setBypass();
         return;
      }
      const float inv = 1.0f / a0;
      mB0 = b0 * inv;
      mB1 = b1 * inv;
      mB2 = b2 * inv;
      mA1 = a1 * inv;
      mA2 = a2 * inv;
   }

   float mB0 = 1.0f, mB1 = 0.0f, mB2 = 0.0f, mA1 = 0.0f, mA2 = 0.0f;
   float mX1 = 0.0f, mX2 = 0.0f, mY1 = 0.0f, mY2 = 0.0f;
};

// A spectral tilt about a pivot: brighter above it and darker below by the same
// amount, with the pivot itself left where it was. Two gentle shelves facing
// opposite ways, which over the four octaves either side of the pivot tracks a
// true constant slope to within about a decibel and costs two biquads instead
// of a filterbank.
//
// This is the shape a measured octave-band curve reduces to once its overall
// level is taken out, so it is what a plugin reaches for when a profile says
// "5 dB per octave darker than the reference".
class Tilt {
public:
   void reset() {
      mLow.reset();
      mHigh.reset();
   }

   // `dbPerOct` is positive for brighter. The shelves are placed two octaves
   // either side of the pivot, so each carries two octaves' worth of the slope.
   void set(float pivotHz, float dbPerOct, float sampleRate) {
      const float half = clampv(dbPerOct, -12.0f, 12.0f) * 2.0f;
      mLow.setLowShelf(pivotHz * 0.25f, -half, sampleRate, 0.5f);
      mHigh.setHighShelf(pivotHz * 4.0f, half, sampleRate, 0.5f);
   }

   inline float tick(float in) { return mHigh.tick(mLow.tick(in)); }

private:
   Biquad mLow, mHigh;
};

} // namespace plugincore
