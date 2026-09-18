#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "fastmath.h"
#include "filters.h"

namespace plugincore {

// Delay line over a power-of-two buffer, with integer and fractional reads.
// Read before write within a tick: read(d) then returns the sample written d
// ticks ago.
class DelayLine {
public:
   void allocate(size_t maxSamples) {
      size_t n = 16;
      while (n < maxSamples + 4)
         n <<= 1;
      mBuffer.assign(n, 0.0f);
      mMask = n - 1;
      mWrite = 0;
   }

   void clear() { std::fill(mBuffer.begin(), mBuffer.end(), 0.0f); }

   size_t capacity() const { return mBuffer.size() - 4; }

   inline float read(size_t delay) const { return mBuffer[(mWrite - delay) & mMask]; }

   inline float readFrac(float delay) const {
      const size_t i = static_cast<size_t>(delay);
      const float frac = delay - static_cast<float>(i);
      const float a = mBuffer[(mWrite - i) & mMask];
      const float b = mBuffer[(mWrite - i - 1) & mMask];
      return a + frac * (b - a);
   }

   inline void write(float v) {
      mBuffer[mWrite] = v;
      mWrite = (mWrite + 1) & mMask;
   }

private:
   std::vector<float> mBuffer;
   size_t mMask = 0, mWrite = 0;
};

// Schroeder allpass, used to diffuse the input before it enters the tank so
// a dense stream of events does not smear into a single flutter.
class Allpass {
public:
   void allocate(size_t maxSamples) { mLine.allocate(maxSamples); }
   void clear() { mLine.clear(); }
   void setDelay(size_t samples) { mDelay = clampv(samples, size_t(1), mLine.capacity()); }
   void setGain(float g) { mGain = g; }

   inline float tick(float in) {
      const float d = mLine.read(mDelay);
      const float v = in + mGain * d;
      mLine.write(v);
      return d - mGain * v;
   }

private:
   DelayLine mLine;
   size_t mDelay = 1;
   float mGain = 0.5f;
};

// The space the sound happens in: a room, a cave, a courtyard.
//
// Two stages, both derived from one physical size. Space Size sets the
// dimension of the room, in metres; from that follow the distances the first
// reflections travel and the mean free path that the late tank's delays are
// built from. Space Damping sets the absorption of the surfaces. Sabine's law
// then gives the decay time -- a small room cannot ring for ten seconds and a
// bare stone hall cannot be dead -- and the decay is frequency dependent the
// way real rooms are: surfaces absorb the mids and highs more than the lows,
// and the air itself eats the top end over the distance the sound has to travel
// before it dies.
//
// Early reflections: eight discrete taps per channel at distances that are
// fixed fractions of the room dimension. In a real cave these are the slaps you
// hear -- the reference recording of one has a reflection 55 ms after each impact
// only 7 dB below it -- and a diffuse tank cannot make them; it can only smear
// them.
//
// Late tail: eight delay lines mixed through an orthonormal Hadamard matrix,
// each with its own frequency-dependent loss so that every line decays at the
// same rate per unit of time whatever its length (Jot). Four lines are slowly
// modulated by a fraction of a millisecond, which is enough to break up the
// metallic modes an unmodulated tank rings with when it is asked for a long
// decay, and far too little to be heard as pitch movement.
class Space {
public:
   // The default suits a source with little energy below ~50 Hz.
   static constexpr float kDefaultLoopHighpassHz = 35.0f;

   static constexpr int kLines = 8;
   static constexpr int kTaps = 8;

   // `loopHighpassHz` is a voicing decision that belongs to the plugin: it is
   // what stops the tank accumulating DC, and it must sit below the lowest
   // frequency the source can produce. Rain wants it an octave or so above
   // what thunder wants.
   void prepare(float sampleRate, float loopHighpassHz = kDefaultLoopHighpassHz) {
      mSampleRate = sampleRate;
      mLoopHighpassHz = loopHighpassHz;
      const size_t maxTank =
         static_cast<size_t>(sampleRate * (kMaxLineSec + 2.0f * kModDepthMaxSec)) + 8;
      for (auto &l : mLines)
         l.allocate(maxTank);
      const size_t maxEarly = static_cast<size_t>(sampleRate * kMaxEarlySec) + 8;
      for (auto &e : mEarly)
         e.allocate(maxEarly);
      const size_t maxAp = static_cast<size_t>(sampleRate * kMaxDiffuserSec) + 8;
      for (auto &a : mDiffusers) {
         a.allocate(maxAp);
         a.setGain(0.62f);
      }
      for (int i = 0; i < kLines; ++i) {
         mLoopHp[i].setCutoff(mLoopHighpassHz, sampleRate);
         mShelfLp[i].setCutoff(kShelfHz, sampleRate);
      }
      for (auto &lp : mEarlyLp)
         lp.reset();
      clear();
      update();
   }

   void clear() {
      for (int i = 0; i < kLines; ++i) {
         mLines[i].clear();
         mShelfLp[i].reset();
         mAirLp[i].reset();
         mLoopHp[i].reset();
         // The modulation is playing state too: a fixed Seed promises the same
         // output after a reset, and that includes where the tank's lines are in
         // their slow wander.
         mLfoPhase[i] = static_cast<float>(i) / kLines;
      }
      for (auto &e : mEarly)
         e.clear();
      for (auto &a : mDiffusers)
         a.clear();
      for (auto &lp : mEarlyLp)
         lp.reset();
   }

   void setSize(float size) {
      mSize = clampv(size, 0.0f, 1.0f);
      update();
   }

   // How enclosed the space is, 0..1. Early reflections are what tell the ear
   // there are walls nearby: eight discrete taps at fixed fractions of the room
   // is a room, and no amount of tail will stop it sounding like one. Outdoors
   // there is nothing close enough to reflect, so the early field is what has to
   // go -- what is left is the diffuse tail and the air. 1 is a room, 0 is open
   // ground with the sky above it.
   void setEnclosure(float enclosure) {
      const float e = clampv(enclosure, 0.0f, 1.0f);
      if (e == mEnclosure)
         return;
      mEnclosure = e;
      update();
   }

   void setDamping(float damping) {
      mDamping = clampv(damping, 0.0f, 1.0f);
      update();
   }

   // Reverberation time at low frequencies, which is the longest and so the
   // one a host needs to know about.
   float decaySeconds() const { return mRtLow; }
   float roomMetres() const { return mRoomM; }

   inline void tick(float inL, float inR, float &outL, float &outR) {
      // --- Early reflections. A little of each channel reaches the other's
      // line: reflections come off every wall, not only the near one.
      const float eL = 0.85f * inL + 0.15f * inR;
      const float eR = 0.15f * inL + 0.85f * inR;
      float erL = 0.0f, erR = 0.0f;
      for (int k = 0; k < kTaps; ++k) {
         erL += mTapGain[0][k] * mEarly[0].read(mTapDelay[0][k]);
         erR += mTapGain[1][k] * mEarly[1].read(mTapDelay[1][k]);
      }
      const float preL = mEarly[0].read(mPreDelay);
      const float preR = mEarly[1].read(mPreDelay);
      mEarly[0].write(eL);
      mEarly[1].write(eR);
      erL = mEarlyLp[0].tick(erL);
      erR = mEarlyLp[1].tick(erR);

      // --- Tank input: the pre-delayed direct sound plus the reflections,
      // diffused so that a dense stream does not enter as a coherent flutter.
      const float tL = mDiffusers[0].tick(mDiffusers[2].tick(0.6f * preL + 0.5f * erL));
      const float tR = mDiffusers[1].tick(mDiffusers[3].tick(0.6f * preR + 0.5f * erR));

      // --- Late tank.
      float a[kLines];
      for (int i = 0; i < kLines; ++i) {
         float delay = mLineDelay[i];
         if (mModDepth[i] > 0.0f) {
            delay += mModDepth[i] * sin2pi(mLfoPhase[i]);
            mLfoPhase[i] += mLfoInc[i];
            if (mLfoPhase[i] >= 1.0f)
               mLfoPhase[i] -= 1.0f;
            a[i] = mLines[i].readFrac(delay);
         } else {
            a[i] = mLines[i].read(static_cast<size_t>(delay));
         }
      }

      // Orthonormal 8x8 Hadamard, as three butterfly stages.
      const float s0 = a[0] + a[1], d0 = a[0] - a[1];
      const float s1 = a[2] + a[3], d1 = a[2] - a[3];
      const float s2 = a[4] + a[5], d2 = a[4] - a[5];
      const float s3 = a[6] + a[7], d3 = a[6] - a[7];
      const float t0 = s0 + s1, t1 = d0 + d1, t2 = s0 - s1, t3 = d0 - d1;
      const float t4 = s2 + s3, t5 = d2 + d3, t6 = s2 - s3, t7 = d2 - d3;
      constexpr float n = 0.35355339f; // 1 / sqrt(8)
      const float m[kLines] = {(t0 + t4) * n, (t1 + t5) * n, (t2 + t6) * n, (t3 + t7) * n,
                               (t0 - t4) * n, (t1 - t5) * n, (t2 - t6) * n, (t3 - t7) * n};

      for (int i = 0; i < kLines; ++i) {
         // Per-line loss: a low shelf (lows ring longer than mids), then the
         // air's own lowpass, then a highpass that keeps the tank from
         // accumulating anything below what a speaker can reproduce.
         float x = mLoopHp[i].tick(m[i]);
         x = mGainMid[i] * x + (mGainLow[i] - mGainMid[i]) * mShelfLp[i].tick(x);
         x = mAirLp[i].tick(x);
         mLines[i].write(((i & 1) ? tR : tL) + x);
      }

      // Left listens to the lines the left input feeds, so an event on one side
      // starts its tail on that side and diffuses across from there.
      outL = erL + 0.35f * (a[0] + a[2] + a[4] + a[6]);
      outR = erR + 0.35f * (a[1] + a[3] + a[5] + a[7]);
   }

private:
   // Room dimension in metres against Space Size: three metres at the bottom,
   // ninety at the top. The reference cave measures as something like sixty.
   static constexpr float kRoomMinM = 3.0f;
   static constexpr float kRoomRangeRatio = 30.0f;
   static constexpr float kSpeedOfSoundMs = 343.0f;
   // Mean free path of a room as a fraction of its dimension: 4V/S, which for a
   // cube is two thirds of the side.
   static constexpr float kMeanFreePathRatio = 0.6f;
   // Late lines as multiples of the mean free path. Spread over nearly three
   // to one, and not in any simple ratio to each other.
   static constexpr float kLineRatio[kLines] = {0.557f, 0.669f, 0.793f, 0.904f,
                                                1.041f, 1.184f, 1.335f, 1.517f};
   static constexpr float kMaxLineSec = 0.30f;
   static constexpr float kMaxEarlySec = 0.30f;
   static constexpr float kMaxDiffuserSec = 0.03f;
   // Modulation of four of the lines: slow, and a fraction of a millisecond.
   static constexpr float kModDepthMaxSec = 0.0006f;
   static constexpr float kModRateHz[kLines] = {0.113f, 0.0f, 0.171f, 0.0f,
                                                0.083f, 0.0f, 0.233f, 0.0f};
   // Early reflection distances as fractions of the room dimension, and their
   // strengths. The fifth is the slap: in the reference cave it arrives 55 ms
   // after the impact at -7 dB and is the most audible thing about the room.
   static constexpr float kTapFrac[kTaps] = {0.075f, 0.13f, 0.19f, 0.245f,
                                             0.30f,  0.41f, 0.55f, 0.72f};
   static constexpr float kTapGain[kTaps] = {0.55f, 0.36f, 0.42f, 0.28f,
                                             0.50f, 0.30f, 0.24f, 0.17f};
   // Absorption of the surfaces at low frequencies against Space Damping, and
   // how much shorter the mid and high decay is than the low one. Sabine:
   // RT60 = 0.161 V / (S alpha), with V/S = D/6 for a cube.
   static constexpr float kAlphaMin = 0.07f;
   static constexpr float kAlphaRange = 0.7f;
   static constexpr float kTiltMax = 0.5f;
   static constexpr float kTiltRange = 0.3f;
   static constexpr float kShelfHz = 600.0f;
   // Air absorption at the 6 kHz reference, in dB per second of travel: about
   // 0.05 dB/m, which is damp air, times the speed
   // of sound.
   static constexpr float kAirRefHz = 6000.0f;
   static constexpr float kAirDbPerSec = 17.0f;
   float mLoopHighpassHz = kDefaultLoopHighpassHz;
   static constexpr float kRtMinSec = 0.08f;
   static constexpr float kRtMaxSec = 20.0f;

   void update() {
      const float D = kRoomMinM * std::pow(kRoomRangeRatio, mSize);
      mRoomM = D;
      const float alpha = kAlphaMin + kAlphaRange * std::pow(mDamping, 1.5f);
      mRtLow = clampv(0.161f * D / (6.0f * alpha), kRtMinSec, kRtMaxSec);
      const float rtMid = mRtLow * (kTiltMax - kTiltRange * mDamping);
      const float rtHigh = 1.0f / (1.0f / rtMid + kAirDbPerSec / 60.0f);

      // --- Early reflections. The right line's taps sit a few per cent off the
      // left's, so the two channels decorrelate without either being wrong.
      const float unit = D / kSpeedOfSoundMs * mSampleRate;
      const size_t earlyCap = mEarly[0].capacity();
      const float absorb = 1.0f - 0.45f * mDamping;
      for (int k = 0; k < kTaps; ++k) {
         const float dl = kTapFrac[k] * unit;
         const float dr = kTapFrac[k] * unit * ((k & 1) ? 1.031f : 0.972f);
         mTapDelay[0][k] = clampv(static_cast<size_t>(dl), size_t(1), earlyCap);
         mTapDelay[1][k] = clampv(static_cast<size_t>(dr), size_t(1), earlyCap);
         // Swap two gains between the channels so the patterns differ.
         const int kr = (k == 1) ? 2 : (k == 2) ? 1 : k;
         mTapGain[0][k] = kTapGain[k] * absorb * mEnclosure;
         mTapGain[1][k] = kTapGain[kr] * absorb * mEnclosure;
      }
      mPreDelay = mTapDelay[0][0];
      const float earlyCutoff = clampv(16000.0f * std::exp2(-3.0f * mDamping), 800.0f,
                                       0.45f * mSampleRate);
      for (auto &lp : mEarlyLp)
         lp.setCutoff(earlyCutoff, mSampleRate);

      // --- Input diffusion, scaled gently with the room.
      static const float kApMs[4] = {5.31f, 8.73f, 6.47f, 9.91f};
      const float apScale = clampv(0.5f + 0.06f * D, 0.5f, 2.5f);
      for (int i = 0; i < 4; ++i)
         mDiffusers[i].setDelay(static_cast<size_t>(kApMs[i] * apScale * 0.001f * mSampleRate));

      // --- Late tank.
      const float meanSec = kMeanFreePathRatio * D / kSpeedOfSoundMs;
      for (int i = 0; i < kLines; ++i) {
         const float sec = clampv(kLineRatio[i] * meanSec, 0.002f, kMaxLineSec);
         const float depth = kModRateHz[i] > 0.0f
                                ? std::min(kModDepthMaxSec, 0.08f * sec) * mSampleRate
                                : 0.0f;
         mModDepth[i] = depth;
         mLineDelay[i] = std::max(sec * mSampleRate, depth + 2.0f);
         mLfoInc[i] = kModRateHz[i] / mSampleRate;

         // Gain per pass for each decay time: 10^(-3 T / RT60).
         mGainLow[i] = std::pow(10.0f, -3.0f * sec / mRtLow);
         mGainMid[i] = std::pow(10.0f, -3.0f * sec / rtMid);
         const float gHigh = std::pow(10.0f, -3.0f * sec / rtHigh);
         // One-pole whose loss at the reference frequency takes the mid gain
         // down to the high one: |H| = r  =>  fc = f r / sqrt(1 - r^2).
         const float r = clampv(gHigh / std::max(mGainMid[i], 1.0e-6f), 0.05f, 0.9999f);
         const float fc = clampv(kAirRefHz * r / std::sqrt(1.0f - r * r), 500.0f,
                                 0.45f * mSampleRate);
         mAirLp[i].setCutoff(fc, mSampleRate);
      }
   }

   DelayLine mLines[kLines];
   DelayLine mEarly[2];
   Allpass mDiffusers[4];
   OnePoleLp mShelfLp[kLines], mAirLp[kLines], mEarlyLp[2];
   OnePoleHp mLoopHp[kLines];
   float mLineDelay[kLines] = {};
   float mGainLow[kLines] = {}, mGainMid[kLines] = {};
   float mModDepth[kLines] = {}, mLfoInc[kLines] = {}, mLfoPhase[kLines] = {};
   size_t mTapDelay[2][kTaps] = {};
   float mTapGain[2][kTaps] = {};
   size_t mPreDelay = 1;
   float mSampleRate = 48000.0f;
   float mSize = 0.5f, mDamping = 0.5f;
   float mEnclosure = 1.0f;
   float mRtLow = 1.0f, mRoomM = 16.0f;
};

} // namespace plugincore
