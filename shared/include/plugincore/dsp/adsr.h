#pragma once

#include "fastmath.h"

namespace plugincore {

// Analogue-style ADSR: linear-ish attack via a one-pole overshoot target,
// exponential decay and release. Drives a noise bed level, and the amplitude
// stamped onto each voice as it is spawned.
class Adsr {
public:
   enum class Stage { Idle, Attack, Decay, Sustain, Release };

   void reset() {
      mLevel = 0.0f;
      mStage = Stage::Idle;
   }

   void setParams(float attackSec, float decaySec, float sustain, float releaseSec,
                  float sampleRate) {
      mAttackCoef = onePoleCoef(attackSec * 0.4f, sampleRate);
      mDecayCoef = onePoleCoef(decaySec * 0.35f, sampleRate);
      mReleaseCoef = onePoleCoef(releaseSec * 0.35f, sampleRate);
      mSustain = clampv(sustain, 0.0f, 1.0f);
   }

   void gateOn() { mStage = Stage::Attack; }

   void gateOff() {
      if (mStage != Stage::Idle)
         mStage = Stage::Release;
   }

   void kill() { reset(); }

   inline float tick() {
      switch (mStage) {
      case Stage::Idle:
         return 0.0f;
      case Stage::Attack:
         // Aim past 1.0 so the attack ends in finite time with a near-linear shape.
         mLevel += mAttackCoef * (1.2f - mLevel);
         if (mLevel >= 1.0f) {
            mLevel = 1.0f;
            mStage = Stage::Decay;
         }
         break;
      case Stage::Decay:
         mLevel += mDecayCoef * (mSustain - mLevel);
         if (mLevel - mSustain < 1.0e-4f)
            mStage = Stage::Sustain;
         break;
      case Stage::Sustain:
         mLevel = mSustain;
         if (mSustain <= 0.0f)
            mStage = Stage::Idle;
         break;
      case Stage::Release:
         mLevel += mReleaseCoef * (-0.02f - mLevel);
         if (mLevel <= 1.0e-4f) {
            mLevel = 0.0f;
            mStage = Stage::Idle;
         }
         break;
      }
      return mLevel;
   }

   bool isIdle() const { return mStage == Stage::Idle; }
   bool isReleasing() const { return mStage == Stage::Release; }
   float level() const { return mLevel; }

private:
   float mLevel = 0.0f;
   float mSustain = 1.0f;
   float mAttackCoef = 0.01f, mDecayCoef = 0.01f, mReleaseCoef = 0.01f;
   Stage mStage = Stage::Idle;
};

} // namespace plugincore
