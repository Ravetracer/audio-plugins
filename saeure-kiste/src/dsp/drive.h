#pragma once

// The drive stage: the one part of this instrument that is not the TB-303.
//
// Everything in front of it is fixed by the February 1982 service notes. There
// is no schematic for this stage, so it is built the other way round -- from
// the literature. Every model below is an equation out of a published source,
// named at the point it is used:
//
//   [DAFX]    U. Zoelzer (ed.), "DAFX: Digital Audio Effects", 2nd edition,
//             Wiley 2011, chapter 4 "Nonlinear processing" (Dutilleux,
//             Dempwolf, Holters, Zoelzer). Equations 4.13 to 4.15 and the
//             M-files beside them.
//   [Pirkle]  W. Pirkle, "Designing Audio Effect Plugins in C++", 2nd edition,
//             Routledge 2019, chapter 19 "Nonlinear Processing: Distortion,
//             Tube Simulation, and HF Exciters". Tables 19.1 and 19.2,
//             equations 19.1 and 19.2, and the TriodeClassA / ClassATubePre
//             objects in sections 19.12 and 19.13.
//
// The books themselves are not in this repository and must not be: they are
// copyrighted and are not ours to redistribute. What is here is the equations,
// implemented, with the citation beside each one.
//
// Why the models are structured differently from each other, and not merely
// curved differently: because a set of memoryless odd-symmetric clippers all
// sound the same. Every one of them flattens into the same square wave once it
// is driven hard, and level-matching the outputs removes what little is left.
// That was the first attempt at this stage and it was a fair criticism of it.
// What makes these tell each other apart is structure -- a linear region that
// survives (Overdrive), an operating point off centre (Tube), three gain
// stages in series with a filter between them (Valve Stack), a rectifier that
// doubles the fundamental (Rectifier), a quantiser (Crush) -- and their own
// filtering, which no waveshaper alone can give.
//
// Both sources say the same thing about aliasing: a nonlinearity needs
// oversampling ([DAFX] section 4.1.1 and figure 4.5; [Pirkle] section 19.1).
// Every model here runs at twice the sample rate for that reason. Soft Clip is
// the exception and deliberately so -- see the note on it below.

#include <cstdint>

#include "plugincore/dsp/filters.h"

namespace saeurekiste {

using namespace plugincore;

// The models, in the order the parameter lists them. A preset stores the
// *name*, so these may be reordered only at the cost of every preset that
// names one.
enum DriveModel {
   kDriveSoftClip = 0, // the stage this plugin always had
   kDriveOverdrive,    // [DAFX] eq 4.14
   kDriveTube,         // [DAFX] eq 4.13 + M-file 4.4
   kDriveValveStack,   // [Pirkle] 19.12/19.13
   kDriveFuzz,         // [DAFX] eq 4.15 + [Pirkle] FEXP1
   kDriveRectifier,    // [DAFX] 4.3.3, [Pirkle] table 19.2 HWR/FWR
   kDriveCrush,        // [Pirkle] eq 19.1
   kNumDriveModels
};

// Seven, and deliberately not more.
//
// [Pirkle] table 19.2 has a hard clipper in it and it was in this list until
// it was measured: at any drive worth using it is the same spectrum as Soft
// Clip, because a soft clipper driven hard *is* a hard clipper -- the legacy
// shape clamps flat at +/-3. Two models that measure the same are one model
// with two names, which is exactly the fault this set was rebuilt to fix, so
// it went. The same goes for the rest of the sources' waveshaper tables: they
// are a dozen ways to draw one S-curve, and one S-curve is enough.

// Two times oversampling around the nonlinearity, which is what both sources
// prescribe: zero-stuffing and an interpolation lowpass on the way up, a
// band-limiting lowpass and decimation on the way down ([DAFX] figure 4.5).
//
// Two cascaded state-variable lowpasses per direction, 24 dB/octave at 0.45 of
// the base rate. It is not a steep half-band filter and does not pretend to
// be: it removes the loudest of what a hard nonlinearity folds back, which is
// the difference between "obviously digital" and "distortion".
class Oversampler2x {
public:
   void prepare(double baseRate) {
      const float twice = static_cast<float>(baseRate * 2.0);
      const float corner = static_cast<float>(baseRate * 0.45);
      mUpA.setCutoff(corner, 0.0f, twice);
      mUpB.setCutoff(corner, 0.0f, twice);
      mDownA.setCutoff(corner, 0.0f, twice);
      mDownB.setCutoff(corner, 0.0f, twice);
      reset();
   }

   void reset() {
      mUpA.reset();
      mUpB.reset();
      mDownA.reset();
      mDownB.reset();
   }

   // One input sample in, one out, with `shape` applied at twice the rate.
   // The factor of two on the way in is the gain a zero-stuffed upsampler
   // loses; the two half-rate samples are filtered on the way back down and
   // the second one is kept.
   template <typename Shape> inline float tick(float x, Shape &&shape) {
      const float a = shape(mUpB.lowpass(mUpA.lowpass(2.0f * x)));
      const float b = shape(mUpB.lowpass(mUpA.lowpass(0.0f)));
      mDownB.lowpass(mDownA.lowpass(a));
      return mDownB.lowpass(mDownA.lowpass(b));
   }

private:
   Svf mUpA, mUpB, mDownA, mDownB;
};

// The stage itself. One per voice; the engine owns it.
class DriveStage {
public:
   void prepare(double sampleRate);
   void reset();

   // `drive` and `bias` are 0..1 and -1..+1 as the parameters give them;
   // `mix` is 0..1, where 0 is a bypass and 1 is the stage alone.
   void setParams(int model, float drive, float bias, float mix);

   inline float tick(float x) {
      if (mMix <= 0.0f)
         return x;
      const float wet = process(x);
      return mMix >= 1.0f ? wet : x + (wet - x) * mMix;
   }

   // The shape on its own, at the current settings and without the mix or the
   // oversampling: what the self-test measures a transfer curve with.
   float shapeOnly(float x) const;

private:
   float process(float x);

   // The waveshapers. Every one of them is an equation from the sources named
   // at the top of this file; see drive.cpp for which, and why each one is
   // shaped the way it is.
   static float softClipLegacy(float x);
   static float overdriveDafx(float x);
   float tubeDafx(float x) const;
   static float fuzzDafx(float x, float gain, float asymmetry);
   float rectifier(float x) const;
   float crush(float x) const;
   float valveStage(float x, float bias) const;

   double mSampleRate = 48000.0;
   int mModel = kDriveSoftClip;
   float mDrive = 0.2f;
   float mBias = 0.0f;
   float mMix = 1.0f;

   // Per-model gain staging and level matching. The peak is measured from the
   // model's own curve rather than assumed -- a folder's peak is in the middle
   // of its range, not at the end of it.
   float mPre = 1.0f;
   float mMakeup = 1.0f;
   float mTrim = 1.0f;

   // Tube: the work point Q and the distortion figure of [DAFX] eq 4.13, and
   // the two filters of the M-file beside it.
   float mTubeQ = -0.2f;
   float mTubeDist = 8.0f;
   float mTubeQOffset = 0.0f; // the constant term of the equation, precomputed
   OnePoleHp mTubeHp;
   OnePoleLp mTubeLp;

   // Valve Stack: four class-A triode stages, each with the DC-removing
   // highpass and the cathode-bypass low shelf of [Pirkle] 19.12, and the
   // two-band shelving EQ [Pirkle] 19.13 puts between the third and the
   // fourth. Four is the book's own NUM_TUBES.
   static constexpr int kValveStages = 4;
   OnePoleHp mValveHp[kValveStages];
   OnePoleLp mValveShelf[kValveStages];
   OnePoleLp mValveEqLow;
   OnePoleHp mValveEqHigh;
   float mValveSat = 1.0f;

   // Fuzz: the asymmetry both sources put in it.
   float mFuzzGain = 1.0f;
   float mFuzzAsym = 0.0f;

   // Rectifier: how much of the rectified signal is mixed with the straight
   // one, and the highpass that takes the offset rectification leaves behind.
   float mRectAmount = 0.0f;
   OnePoleHp mRectHp;

   // Crush.
   float mCrushStep = 1.0f / 32768.0f;
   float mCrushOffset = 0.0f;

   Oversampler2x mOversampler;
};

} // namespace saeurekiste
