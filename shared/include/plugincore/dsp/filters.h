#pragma once

#include "fastmath.h"

namespace plugincore {

// Topology-preserving-transform state variable filter (Andrew Simper / Vadim
// Zavalishin). One tan() per coefficient update, then 8 multiplies per sample
// for simultaneous LP/BP/HP outputs. Stable up to Nyquist, which matters
// because resonators get placed all over the spectrum.
class Svf {
public:
   void reset() {
      mIc1 = 0.0f;
      mIc2 = 0.0f;
   }

   void setCutoff(float cutoffHz, float resonance, float sampleRate) {
      const float f = clampv(cutoffHz / sampleRate, 1.0e-5f, 0.49f);
      const float g = std::tan(3.14159265358979f * f);
      // resonance 0..1 maps to k 2..0.02 (k = 1/Q)
      mK = clampv(2.0f - 1.98f * resonance, 0.02f, 2.0f);
      mA1 = 1.0f / (1.0f + g * (g + mK));
      mA2 = g * mA1;
      mA3 = g * mA2;
   }

   inline void tick(float in, float &lp, float &bp, float &hp) {
      const float v3 = in - mIc2;
      const float v1 = mA1 * mIc1 + mA2 * v3;
      const float v2 = mIc2 + mA2 * mIc1 + mA3 * v3;
      mIc1 = 2.0f * v1 - mIc1;
      mIc2 = 2.0f * v2 - mIc2;
      lp = v2;
      bp = v1;
      hp = in - mK * v1 - v2;
   }

   inline float lowpass(float in) {
      float lp, bp, hp;
      tick(in, lp, bp, hp);
      return lp;
   }

   // Constant-peak-gain bandpass: bp * k normalises the resonant boost, so a
   // resonator does not get louder as its Q rises.
   inline float bandpassNormalised(float in) {
      float lp, bp, hp;
      tick(in, lp, bp, hp);
      return bp * mK;
   }

   inline float notch(float in) {
      float lp, bp, hp;
      tick(in, lp, bp, hp);
      return in - mK * bp;
   }

   inline float k() const { return mK; }
   // Whether the state still holds anything above `floor`: a resonator keeps
   // ringing after its input has stopped, and stops mattering when this is false.
   inline bool ringing(float floor) const {
      return std::fabs(mIc1) > floor || std::fabs(mIc2) > floor;
   }

private:
   float mIc1 = 0.0f, mIc2 = 0.0f;
   float mK = 1.0f, mA1 = 0.0f, mA2 = 0.0f, mA3 = 0.0f;
};

// One-pole lowpass. Used for air absorption, reverb damping and smoothing
// control signals.
class OnePoleLp {
public:
   void reset() { mZ = 0.0f; }
   void setCoef(float coef) { mCoef = clampv(coef, 0.0f, 1.0f); }
   void setCutoff(float cutoffHz, float sampleRate) {
      const float f = clampv(cutoffHz / sampleRate, 1.0e-5f, 0.49f);
      mCoef = 1.0f - std::exp(-6.283185307f * f);
   }
   inline float tick(float in) {
      mZ += mCoef * (in - mZ);
      return mZ;
   }
   inline float value() const { return mZ; }

private:
   float mZ = 0.0f;
   float mCoef = 0.5f;
};

// One-pole highpass, for keeping DC and sub rumble out of a noise bed.
class OnePoleHp {
public:
   void reset() { mZ = 0.0f; }
   void setCutoff(float cutoffHz, float sampleRate) {
      const float f = clampv(cutoffHz / sampleRate, 1.0e-5f, 0.49f);
      mCoef = 1.0f - std::exp(-6.283185307f * f);
   }
   inline float tick(float in) {
      mZ += mCoef * (in - mZ);
      return in - mZ;
   }

private:
   float mZ = 0.0f;
   float mCoef = 0.01f;
};

// Two cascaded one-poles: 12 dB/oct. Used where a 6 dB/oct slope is not enough
// to get rid of what is below the corner -- a noise bed's low end, or the
// radiation rolloff of an individual source.
class Hp2 {
public:
   void reset() {
      mA.reset();
      mB.reset();
   }
   void setCutoff(float cutoffHz, float sampleRate) {
      mA.setCutoff(cutoffHz, sampleRate);
      mB.setCutoff(cutoffHz, sampleRate);
   }
   inline float tick(float in) { return mB.tick(mA.tick(in)); }

private:
   OnePoleHp mA, mB;
};

// Two cascaded one-pole lowpasses: 12 dB/oct. The air absorption of a shock
// wave that has travelled kilometres is steeper than a single pole, and the
// N-wave's own spectrum only falls at 6 dB/oct, so one pole leaves a distant
// thunder with a top end it cannot have.
class Lp2 {
public:
   void reset() {
      mA.reset();
      mB.reset();
   }
   void setCutoff(float cutoffHz, float sampleRate) {
      mA.setCutoff(cutoffHz, sampleRate);
      mB.setCutoff(cutoffHz, sampleRate);
   }
   inline float tick(float in) { return mB.tick(mA.tick(in)); }

private:
   OnePoleLp mA, mB;
};

} // namespace plugincore
