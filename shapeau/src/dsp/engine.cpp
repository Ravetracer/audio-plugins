#include "dsp/engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "plugincore/dsp/fastmath.h"

namespace shapeau {

namespace {

// The quality factor of each pole pair in a fourth-order Linkwitz-Riley split
// is a Butterworth's 1/sqrt(2). plugincore::Svf takes resonance 0..1 mapped
// onto k = 2 - 1.98 * resonance, and k = 1/Q, so this is where Q lands.
constexpr float kLr4Resonance = 0.29575f;

// The longest look-back the time stage can hold, at the highest sample rate a
// host is likely to ask for. The buffer is this plus a little slack.
constexpr float kMaxTimeRange = 2.0f;
constexpr int kMaxDelaySamples = 192000 * 2 + 64;

// How often the modulation is sampled. The curves are read once per control
// block and interpolated inside it, which is far above any drawn movement and
// keeps a tan() out of the per-sample path.
constexpr uint32_t kControl = 16;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float dbToGainf(float db) { return std::pow(10.0f, db * 0.05f); }

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// plugincore::Svf offers lowpass, bandpass and notch convenience calls but not a
// highpass one; the highpass output is already computed inside tick(), so this
// is one line rather than a change to the shared header.
inline float highpassOf(plugincore::Svf &f, float in) {
   float lp = 0.0f, bp = 0.0f, hp = 0.0f;
   f.tick(in, lp, bp, hp);
   return hp;
}

} // namespace

void Engine::prepare(double sampleRate, uint32_t /*maxFrames*/) {
   mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
   for (int b = 0; b < kNumBands; ++b) {
      mBands[b].length = kMaxDelaySamples;
      for (int c = 0; c < 2; ++c) {
         mBands[b].delay[c].assign(static_cast<size_t>(kMaxDelaySamples), 0.0f);
         mBands[b].tone[c].setCutoff(800.0f, static_cast<float>(mSampleRate));
         mBands[b].dc[c].setCutoff(12.0f, static_cast<float>(mSampleRate));
      }
      mBands[b].write = 0;
   }
   mSplitLowCached = -1.0f;
   mSplitHighCached = -1.0f;
   reset();
}

void Engine::reset() {
   for (int b = 0; b < kNumBands; ++b) {
      mBands[b].filter[0].reset();
      mBands[b].filter[1].reset();
      mBands[b].tone[0].reset();
      mBands[b].tone[1].reset();
      mBands[b].dc[0].reset();
      mBands[b].dc[1].reset();
      mBands[b].smoothedMod = 1.0f;
      mBands[b].smoothedOffset = 0.0f;
      mBands[b].smoothedCrush = 0.0f;
      mBands[b].smoothedDrive = -1.0f;
      mBands[b].smoothedFilter = -1.0f;
      mBands[b].holdPhase = 1.0f;
      mBands[b].hold[0] = 0.0f;
      mBands[b].hold[1] = 0.0f;
      mBands[b].smoothedPan = 0.5f;
      mBands[b].smoothedWidth = 1.0f;
      for (int c = 0; c < 2; ++c)
         std::fill(mBands[b].delay[c].begin(), mBands[b].delay[c].end(), 0.0f);
      mBands[b].write = 0;
   }
   mFreeBeats = 0.0;
   for (int m = 0; m < kNumModules; ++m)
      mTrigElapsed[m] = -1.0;
   mMidiTrigCount = 0;
   mAudioTrigCount = 0;
   for (int c = 0; c < 2; ++c) {
      mDetLp[c][0].reset();
      mDetLp[c][1].reset();
   }
   mDetEnv = 0.0f;
   mDetArmed = true;
   mDetSince = 0xffffffffu;
   for (int c = 0; c < 2; ++c) {
      for (int s = 0; s < 2; ++s) {
         mSplitLowLp[c][s].reset();
         mSplitLowHp[c][s].reset();
         mSplitHighLp[c][s].reset();
         mSplitHighHp[c][s].reset();
      }
   }
   mSilent = true;
}

void Engine::setParams(const EngineParams &params) { mParams = params; }

void Engine::publishCurves(const CurveSet &set) {
   // Writer side of the seqlock: an odd count says "being written", and the
   // audio thread retries rather than reading half a shape.
   mCurveSeq.fetch_add(1, std::memory_order_acq_rel);
   std::memcpy(&mCurves, &set, sizeof(CurveSet));
   mCurveSeq.fetch_add(1, std::memory_order_acq_rel);
}

void Engine::readCurves(CurveSet &into) {
   for (int attempt = 0; attempt < 64; ++attempt) {
      const uint32_t before = mCurveSeq.load(std::memory_order_acquire);
      if (before & 1u)
         continue;
      std::memcpy(&into, &mCurves, sizeof(CurveSet));
      const uint32_t after = mCurveSeq.load(std::memory_order_acquire);
      if (before == after)
         return;
   }
   // Never lost in practice: the writer is a GUI edit or a preset load, and
   // both are far apart. Keeping the previous block's shapes is correct.
}

float Engine::smootherCoef(float smooth, double sampleRate) {
   // 0 is instant; 1 is about 30 ms, enough to round off the corner of a drawn
   // step without smearing a sixteenth note at 130 BPM.
   const float seconds = 0.0003f + clampf(smooth, 0.0f, 1.0f) * 0.03f;
   const float c = 1.0f - std::exp(-1.0f / (seconds * static_cast<float>(sampleRate)));
   return clampf(c, 0.0f, 1.0f);
}

void Engine::updateSplit() {
   const float sr = static_cast<float>(mSampleRate);
   const float low = clampf(mParams.splitLowHz, 20.0f, 0.4f * sr);
   const float high = clampf(mParams.splitHighHz, low * 1.2f, 0.45f * sr);
   if (low == mSplitLowCached && high == mSplitHighCached)
      return;
   mSplitLowCached = low;
   mSplitHighCached = high;
   for (int c = 0; c < 2; ++c) {
      for (int s = 0; s < 2; ++s) {
         mSplitLowLp[c][s].setCutoff(low, kLr4Resonance, sr);
         mSplitLowHp[c][s].setCutoff(low, kLr4Resonance, sr);
         mSplitHighLp[c][s].setCutoff(high, kLr4Resonance, sr);
         mSplitHighHp[c][s].setCutoff(high, kLr4Resonance, sr);
      }
   }
}

float Engine::phaseAt(int rate, float phaseOffset, uint32_t frame, double beats, double bpm) const {
   const double cycle = beatsForRate(rate);
   const double advance = static_cast<double>(frame) * (bpm > 0.0 ? bpm : 120.0) / 60.0 /
                          mSampleRate;
   const double position = (beats >= 0.0 ? beats : mFreeBeats) + advance;
   double phase = std::fmod(position / cycle + static_cast<double>(phaseOffset), 1.0);
   if (phase < 0.0)
      phase += 1.0;
   return static_cast<float>(phase);
}

void Engine::cycleOf(int module, int &rate, float &offset) const {
   switch (module) {
   case kModuleVolume: rate = mParams.volRate; offset = mParams.volPhase; break;
   case kModuleFilter: rate = mParams.fltRate; offset = mParams.fltPhase; break;
   case kModuleDrive: rate = mParams.drvRate; offset = mParams.drvPhase; break;
   case kModuleCrush: rate = mParams.crsRate; offset = mParams.crsPhase; break;
   case kModuleTime: rate = mParams.timRate; offset = mParams.timPhase; break;
   case kModuleWidth: rate = mParams.widRate; offset = mParams.widPhase; break;
   case kModulePan: rate = mParams.panRate; offset = mParams.panPhase; break;
   default: rate = kRate4; offset = 0.0f; break;
   }
}

double Engine::triggerElapsed(int module, uint32_t frame, double bpm) const {
   const bool midi = mParams.trigMode[module] == kTrigMidi;
   const uint32_t *list = midi ? mMidiTrig : mAudioTrig;
   const uint32_t count = midi ? mMidiTrigCount : mAudioTrigCount;
   const double perSample = (bpm > 0.0 ? bpm : 120.0) / 60.0 / mSampleRate;
   for (uint32_t k = count; k-- > 0;)
      if (list[k] <= frame)
         return static_cast<double>(frame - list[k]) * perSample;
   if (mTrigElapsed[module] < 0.0)
      return -1.0;
   return mTrigElapsed[module] + static_cast<double>(frame) * perSample;
}

float Engine::modulePhase(int module, uint32_t frame, double beats, double bpm,
                          bool *holding) const {
   int rate = kRate4;
   float offset = 0.0f;
   cycleOf(module, rate, offset);
   if (holding)
      *holding = false;
   if (mParams.trigMode[module] == kTrigSync)
      return phaseAt(rate, offset, frame, beats, bpm);

   // A triggered cycle runs at the same tempo-locked rate, counted from the
   // trigger instead of from the start of the song. Before its first trigger,
   // and once a cycle run once has reached its End, it rests there -- which
   // for a duck drawn back up to the top is the untouched signal.
   const double elapsed = triggerElapsed(module, frame, bpm);
   const double end = clampf(mParams.trigEnd[module], 0.0f, 1.0f);
   const bool loop = mParams.trigLoop[module];
   double pos;
   bool held = false;
   if (elapsed < 0.0) {
      pos = loop ? 1.0 : end;
      held = true;
   } else if (loop) {
      pos = std::fmod(elapsed / beatsForRate(rate), 1.0);
   } else {
      pos = elapsed / beatsForRate(rate);
      if (pos >= end) {
         pos = end;
         held = true;
      }
   }
   if (holding)
      *holding = held;
   // Phase shifts the shape under the cycle as it does in Sync. Only past the
   // end does it wrap, so a cycle resting at 1 reads the shape's last point.
   double phase = pos + static_cast<double>(offset);
   if (phase > 1.0)
      phase -= 1.0;
   return static_cast<float>(phase);
}

// The audio trigger: a peak follower after an optional two-pole lowpass,
// firing when it crosses the threshold, re-arming 6 dB below it, and never
// twice within the hold time.
void Engine::detectTriggers(const float *l, const float *r, uint32_t frames) {
   mAudioTrigCount = 0;
   const float sr = static_cast<float>(mSampleRate);
   const bool filtered = mParams.trigFilterHz < 19000.0f;
   if (filtered && mParams.trigFilterHz != mDetLpCached) {
      mDetLpCached = mParams.trigFilterHz;
      for (int c = 0; c < 2; ++c)
         for (int s = 0; s < 2; ++s)
            mDetLp[c][s].setCutoff(mParams.trigFilterHz, sr);
   }
   // 30 ms: slow enough not to fall between the peaks of a 40 Hz kick tail,
   // fast enough to re-arm between sixteenths at 170 BPM.
   const float release = std::exp(-1.0f / (0.03f * sr));
   const float threshold = std::max(mParams.trigThreshold, 1.0e-6f);
   const float rearm = 0.5f * threshold;
   const uint32_t hold = static_cast<uint32_t>(std::max(0.001f, mParams.trigHoldSec) * sr);
   for (uint32_t i = 0; i < frames; ++i) {
      float a = l ? l[i] : 0.0f;
      float b = r ? r[i] : a;
      if (filtered) {
         a = mDetLp[0][1].tick(mDetLp[0][0].tick(a));
         b = mDetLp[1][1].tick(mDetLp[1][0].tick(b));
      }
      const float x = std::max(std::fabs(a), std::fabs(b));
      mDetEnv = x > mDetEnv ? x : mDetEnv * release;
      if (mDetSince != 0xffffffffu)
         ++mDetSince;
      if (mDetArmed && mDetEnv >= threshold && mDetSince >= hold) {
         if (mAudioTrigCount < kMaxTriggers)
            mAudioTrig[mAudioTrigCount++] = i;
         mDetArmed = false;
         mDetSince = 0;
         mScope.triggers.fetch_add(1, std::memory_order_relaxed);
      } else if (!mDetArmed && mDetEnv < rearm) {
         mDetArmed = true;
      }
   }
}

// Carries every triggered module's time since its trigger into the next block.
// A looping cycle keeps only its place in the cycle and one run once stops
// counting at its end, so neither grows without bound over a long session.
void Engine::endTriggerBlock(uint32_t frames, double bpm) {
   for (int m = 0; m < kNumModules; ++m) {
      if (mParams.trigMode[m] == kTrigSync) {
         mTrigElapsed[m] = -1.0;
         continue;
      }
      double elapsed = triggerElapsed(m, frames, bpm);
      if (elapsed >= 0.0) {
         int rate = kRate4;
         float offset = 0.0f;
         cycleOf(m, rate, offset);
         const double cycle = beatsForRate(rate);
         elapsed = mParams.trigLoop[m] ? std::fmod(elapsed, cycle) : std::min(elapsed, cycle);
      }
      mTrigElapsed[m] = elapsed;
   }
}

float Engine::driveShape(int style, float x, float amount) {
   switch (style) {
   case kHardClip:
      return clampf(x, -1.0f, 1.0f);
   case kFold: {
      // Reflecting the signal at +/-1 rather than clamping it: a wavefolder's
      // harmonics, not a clipper's.
      float y = std::fmod(x + 1.0f, 4.0f);
      if (y < 0.0f)
         y += 4.0f;
      y = y > 2.0f ? 4.0f - y : y;
      return y - 1.0f;
   }
   case kRectify:
      // Octave-up: the negative half is turned up, and the DC that leaves is
      // removed by the band's high-pass.
      return std::fabs(x);
   case kSineFold:
      // A bipolar fold that keeps the fundamental: sin() turns the amplitude
      // into phase, so the shape never leaves the curve.
      return std::sin(x * 1.5707963f);
   case kCrush: {
      const float bits = 16.0f - 14.0f * clampf(amount, 0.0f, 1.0f);
      const float levels = std::pow(2.0f, bits) - 1.0f;
      return std::round(x * levels) / levels;
   }
   case kSoftClip:
   default:
      return std::tanh(x);
   }
}

double Engine::tailSeconds() const {
   if (!mParams.timOn)
      return 0.0;
   return static_cast<double>(mParams.timRangeSec) + 0.05;
}

void Engine::process(float *left, float *right, uint32_t frames, double beats, double bpm,
                     const float *sideL, const float *sideR, const uint32_t *notes,
                     uint32_t noteCount) {
   if (!left || !right || frames == 0)
      return;
   if (sideL && !sideR)
      sideR = sideL;

   // This block's triggers, before anything writes over the input: the notes
   // as the host sent them, and the hits the detector finds in its source.
   mMidiTrigCount = 0;
   for (uint32_t k = 0; k < noteCount && mMidiTrigCount < kMaxTriggers; ++k) {
      uint32_t at = std::min(notes[k], frames - 1);
      if (mMidiTrigCount > 0)
         at = std::max(at, mMidiTrig[mMidiTrigCount - 1]);
      mMidiTrig[mMidiTrigCount++] = at;
   }
   if (mMidiTrigCount > 0)
      mScope.triggers.fetch_add(mMidiTrigCount, std::memory_order_relaxed);
   if (mParams.trigFromInput)
      detectTriggers(left, right, frames);
   else
      detectTriggers(sideL, sideR, frames);

   CurveSet curves;
   readCurves(curves);
   updateSplit();

   // A host that stops the transport still lets the curves run: a free-running
   // phase at the project's tempo, so an LFO drawn at 130 BPM keeps its rate
   // while the song is paused.
   const double freeStep = (bpm > 0.0 ? bpm : 120.0) / 60.0 / mSampleRate;
   const bool freerun = beats < 0.0;

   // The order the effects run in this block, and whether any of them does.
   bool on[kNumModules];
   for (int m = 0; m < kNumModules; ++m)
      on[m] = moduleOn(m);
   mOrderCount = effectiveChain(curves, on, mOrder);
   bool anyModule = false;
   for (int i = 0; i < mOrderCount; ++i)
      anyModule = anyModule || on[mOrder[i]];
   scopeBlock(frames, beats, bpm);

   if (!anyModule) {
      const float g = mParams.inputGain * mParams.outputGain;
      for (uint32_t i = 0; i < frames; ++i) {
         const float inL = left[i] * mParams.inputGain, inR = right[i] * mParams.inputGain;
         left[i] *= g;
         right[i] *= g;
         if (mScopeOn)
            scopeSample(i, inL, inR, left[i], right[i], sideL ? sideL[i] : 0.0f,
                        sideR ? sideR[i] : 0.0f, -1.0f);
      }
      scopeEnd();
      endTriggerBlock(frames, bpm);
      if (freerun)
         mFreeBeats += freeStep * frames;
      mSilent = false;
      return;
   }

   mVolCoef = smootherCoef(mParams.volSmooth, mSampleRate);
   mDrvCoef = smootherCoef(mParams.drvSmooth, mSampleRate);
   mFltCoef = smootherCoef(mParams.fltSmooth, mSampleRate);
   mTimeCoef = smootherCoef(mParams.timStep, mSampleRate);
   mCrsCoef = smootherCoef(mParams.crsSmooth, mSampleRate);
   mPanCoef = smootherCoef(mParams.panSmooth, mSampleRate);
   mWidCoef = smootherCoef(mParams.widSmooth, mSampleRate);

   const float sr = static_cast<float>(mSampleRate);

   for (uint32_t base = 0; base < frames; base += kControl) {
      const uint32_t n = std::min(kControl, frames - base);

      // Curve values for the start and the end of this control block. The band
      // is the innermost index because each band can have its own shape.
      float yVol0[kNumBands], yVol1[kNumBands];
      float yDrv0[kNumBands], yDrv1[kNumBands];
      float yCrs0[kNumBands], yCrs1[kNumBands];
      float yTim0[kNumBands], yTim1[kNumBands];
      float yPan0[kNumBands], yPan1[kNumBands];
      float yWid0[kNumBands], yWid1[kNumBands];
      float yFlt[kNumBands];

      const float volPhase0 =
         modulePhase(kModuleVolume, base, beats, bpm);
      const float volPhase1 =
         modulePhase(kModuleVolume, base + n, beats, bpm);
      const float drvPhase0 =
         modulePhase(kModuleDrive, base, beats, bpm);
      const float drvPhase1 =
         modulePhase(kModuleDrive, base + n, beats, bpm);
      const float timPhase0 =
         modulePhase(kModuleTime, base, beats, bpm);
      const float timPhase1 =
         modulePhase(kModuleTime, base + n, beats, bpm);
      const float crsPhase0 =
         modulePhase(kModuleCrush, base, beats, bpm);
      const float crsPhase1 =
         modulePhase(kModuleCrush, base + n, beats, bpm);
      const float panPhase0 =
         modulePhase(kModulePan, base, beats, bpm);
      const float panPhase1 =
         modulePhase(kModulePan, base + n, beats, bpm);
      const float widPhase0 =
         modulePhase(kModuleWidth, base, beats, bpm);
      const float widPhase1 =
         modulePhase(kModuleWidth, base + n, beats, bpm);
      const float fltPhase = modulePhase(kModuleFilter, base, beats, bpm);

      for (int b = 0; b < kNumBands; ++b) {
         yVol0[b] = curves.curve[kModuleVolume][b].valueAt(volPhase0);
         yVol1[b] = curves.curve[kModuleVolume][b].valueAt(volPhase1);
         yDrv0[b] = curves.curve[kModuleDrive][b].valueAt(drvPhase0);
         yDrv1[b] = curves.curve[kModuleDrive][b].valueAt(drvPhase1);
         yCrs0[b] = curves.curve[kModuleCrush][b].valueAt(crsPhase0);
         yCrs1[b] = curves.curve[kModuleCrush][b].valueAt(crsPhase1);
         yTim0[b] = curves.curve[kModuleTime][b].valueAt(timPhase0);
         yTim1[b] = curves.curve[kModuleTime][b].valueAt(timPhase1);
         yPan0[b] = curves.curve[kModulePan][b].valueAt(panPhase0);
         yPan1[b] = curves.curve[kModulePan][b].valueAt(panPhase1);
         yWid0[b] = curves.curve[kModuleWidth][b].valueAt(widPhase0);
         yWid1[b] = curves.curve[kModuleWidth][b].valueAt(widPhase1);
         yFlt[b] = curves.curve[kModuleFilter][b].valueAt(fltPhase);

         if (mParams.fltOn) {
            // The cutoff moves once per control block, so its smoothing does
            // too: the per-sample coefficient raised to the block's length.
            BandState &state = mBands[b];
            if (state.smoothedFilter < 0.0f)
               state.smoothedFilter = yFlt[b];
            else
               state.smoothedFilter +=
                  (1.0f - std::pow(1.0f - mFltCoef, static_cast<float>(n))) *
                  (yFlt[b] - state.smoothedFilter);
            const float depth = static_cast<float>(mParams.fltDepthOct);
            const float cutoff = clampf(
               mParams.fltCutHz * std::exp2((state.smoothedFilter - 1.0f) * depth), 20.0f,
               0.45f * sr);
            mBands[b].filter[0].setCutoff(cutoff, mParams.fltRes, sr);
            mBands[b].filter[1].setCutoff(cutoff, mParams.fltRes, sr);
         }
      }

      const float inv = n > 1 ? 1.0f / static_cast<float>(n - 1) : 1.0f;

      for (uint32_t i = 0; i < n; ++i) {
         const uint32_t frame = base + i;
         const float t = static_cast<float>(i) * inv;

         const float l = left[frame] * mParams.inputGain;
         const float r = right[frame] * mParams.inputGain;

         // The three bands, as Linkwitz-Riley fourth order: two identical
         // Butterworth sections give the -6 dB at the crossover that makes the
         // in-phase sum flat.
         float band[kNumBands][2];
         for (int c = 0; c < 2; ++c) {
            const float in = c == 0 ? l : r;
            const float lowHp = highpassOf(mSplitLowHp[c][1], highpassOf(mSplitLowHp[c][0], in));
            const float lowLp = mSplitLowLp[c][1].lowpass(mSplitLowLp[c][0].lowpass(in));
            const float mid = mSplitHighLp[c][1].lowpass(mSplitHighLp[c][0].lowpass(lowHp));
            const float high = highpassOf(mSplitHighHp[c][1], highpassOf(mSplitHighHp[c][0], lowHp));
            band[kBandLow][c] = lowLp;
            band[kBandMid][c] = mid;
            band[kBandHigh][c] = high;
         }

         float outL = 0.0f, outR = 0.0f;
         float scopeInL = l, scopeInR = r, scopeOutL = 0.0f, scopeOutR = 0.0f;
         float scopeTrace = -1.0f;
         for (int b = 0; b < kNumBands; ++b) {
            float bl = band[b][0];
            float br = band[b][1];
            const bool scoped = mScopeOn && b == mScopeTraceBand;
            if (scoped && !mScopeAllBands) {
               scopeInL = bl;
               scopeInR = br;
            }
            if (mParams.bandOn[b]) {
               float y[kNumModules];
               y[kModuleVolume] = lerpf(yVol0[b], yVol1[b], t);
               y[kModuleFilter] = yFlt[b];
               y[kModuleDrive] = lerpf(yDrv0[b], yDrv1[b], t);
               y[kModuleCrush] = lerpf(yCrs0[b], yCrs1[b], t);
               y[kModuleTime] = lerpf(yTim0[b], yTim1[b], t);
               y[kModulePan] = lerpf(yPan0[b], yPan1[b], t);
               y[kModuleWidth] = lerpf(yWid0[b], yWid1[b], t);
               processBand(b, bl, br, y, mParams.fltType[b]);
               if (scoped)
                  scopeTrace = traceOf(mScopeTarget, b, y[mScopeTarget]);
            }
            if (scoped && !mScopeAllBands) {
               scopeOutL = bl;
               scopeOutR = br;
            }
            outL += bl;
            outR += br;
         }

         const float dryL = left[frame], dryR = right[frame];
         const float wetL = outL * mParams.outputGain;
         const float wetR = outR * mParams.outputGain;
         left[frame] = dryL + (wetL - dryL) * mParams.mix;
         right[frame] = dryR + (wetR - dryR) * mParams.mix;
         if (mScopeOn) {
            if (mScopeAllBands) {
               scopeOutL = left[frame];
               scopeOutR = right[frame];
            }
            scopeSample(frame, scopeInL, scopeInR, scopeOutL, scopeOutR,
                        sideL ? sideL[frame] : 0.0f, sideR ? sideR[frame] : 0.0f, scopeTrace);
         }
      }
   }
   scopeEnd();
   endTriggerBlock(frames, bpm);

   if (freerun)
      mFreeBeats += freeStep * frames;
   mSilent = false;
}

// ------------------------------------------------------------------- scope

void Engine::scopeBlock(uint32_t frames, double beats, double bpm) {
   mScopeOn = mScopeActive.load(std::memory_order_relaxed);
   if (!mScopeOn)
      return;
   const int module = std::clamp(mScopeModule.load(std::memory_order_relaxed), 0, kNumModules - 1);
   const int band = std::clamp(mScopeBand.load(std::memory_order_relaxed), 0, kNumBands - 1);
   const bool all = mScopeAll.load(std::memory_order_relaxed);
   if (module != mScopeLastModule || band != mScopeLastBand || all != mScopeLastAll) {
      mScope.clear();
      mScopeBin = -1;
      mScopeLastModule = module;
      mScopeLastBand = band;
      mScopeLastAll = all;
   }
   mScopeTarget = module;
   mScopeTraceBand = band;
   mScopeAllBands = all;

   // Where the selected effect's shape is read at the top of the block, and how
   // far it moves per sample: the same arithmetic phaseAt() does per control
   // block, unrolled so every sample lands in its own place.
   int rate = kRate4;
   float offset = 0.0f;
   cycleOf(module, rate, offset);
   mScopeTriggered = mParams.trigMode[module] != kTrigSync;
   mScopeBeats = beats;
   mScopeBpm = bpm;
   mScopePos0 = phaseAt(rate, offset, 0, beats, bpm);
   mScopeInc = (bpm > 0.0 ? bpm : 120.0) / 60.0 / mSampleRate / beatsForRate(rate);
   (void)frames;
}

void Engine::scopeSample(uint32_t frame, float inL, float inR, float outL, float outR,
                         float sideL, float sideR, float trace) {
   double pos;
   if (mScopeTriggered) {
      // A triggered cycle restarts wherever the trigger fell, so its phase is
      // worked out per sample. While it holds at its End -- or waits for its
      // first trigger -- the playhead parks there and no bin is written: a
      // second of held audio piled into one bin would be one tall spike.
      bool holding = false;
      pos = modulePhase(mScopeTarget, frame, mScopeBeats, mScopeBpm, &holding);
      if (holding) {
         mScope.playhead.store(static_cast<float>(pos), std::memory_order_relaxed);
         return;
      }
   } else {
      pos = mScopePos0 + mScopeInc * frame;
   }
   pos -= std::floor(pos);
   const int bin = std::min(Scope::kBins - 1, static_cast<int>(pos * Scope::kBins));
   const float inLo = std::min(inL, inR), inHi = std::max(inL, inR);
   const float outLo = std::min(outL, outR), outHi = std::max(outL, outR);
   const float sideLo = std::min(sideL, sideR), sideHi = std::max(sideL, sideR);
   if (bin == mScopeBin) {
      mBinInLo = std::min(mBinInLo, inLo);
      mBinInHi = std::max(mBinInHi, inHi);
      mBinOutLo = std::min(mBinOutLo, outLo);
      mBinOutHi = std::max(mBinOutHi, outHi);
      mBinSideLo = std::min(mBinSideLo, sideLo);
      mBinSideHi = std::max(mBinSideHi, sideHi);
      mBinTrace = trace;
      return;
   }
   // A new bin. The one just finished is published; bins the read position
   // skipped over -- a fast rate, or a long block -- take the new sample, so
   // the picture has no gaps. A jump backwards (a loop, a seek) skips nothing,
   // and for a triggered cycle every jump backwards is a restart: read as a
   // wrap, a trigger late in the cycle would smear one sample over its end.
   if (mScopeBin >= 0) {
      scopeEnd();
      const int ahead = (bin - mScopeBin + Scope::kBins) % Scope::kBins;
      if (ahead > 1 && ahead < Scope::kBins / 4 && !(mScopeTriggered && bin < mScopeBin))
         for (int k = (mScopeBin + 1) % Scope::kBins; k != bin; k = (k + 1) % Scope::kBins) {
            mScope.inLo[k].store(inLo, std::memory_order_relaxed);
            mScope.inHi[k].store(inHi, std::memory_order_relaxed);
            mScope.outLo[k].store(outLo, std::memory_order_relaxed);
            mScope.outHi[k].store(outHi, std::memory_order_relaxed);
            mScope.sideLo[k].store(sideLo, std::memory_order_relaxed);
            mScope.sideHi[k].store(sideHi, std::memory_order_relaxed);
            mScope.trace[k].store(trace, std::memory_order_relaxed);
         }
   }
   mScopeBin = bin;
   mBinInLo = inLo;
   mBinInHi = inHi;
   mBinOutLo = outLo;
   mBinOutHi = outHi;
   mBinSideLo = sideLo;
   mBinSideHi = sideHi;
   mBinTrace = trace;
   mScope.playhead.store(static_cast<float>(pos), std::memory_order_relaxed);
}

void Engine::scopeEnd() {
   if (!mScopeOn || mScopeBin < 0)
      return;
   mScope.inLo[mScopeBin].store(mBinInLo, std::memory_order_relaxed);
   mScope.inHi[mScopeBin].store(mBinInHi, std::memory_order_relaxed);
   mScope.outLo[mScopeBin].store(mBinOutLo, std::memory_order_relaxed);
   mScope.outHi[mScopeBin].store(mBinOutHi, std::memory_order_relaxed);
   mScope.sideLo[mScopeBin].store(mBinSideLo, std::memory_order_relaxed);
   mScope.sideHi[mScopeBin].store(mBinSideHi, std::memory_order_relaxed);
   mScope.trace[mScopeBin].store(mBinTrace, std::memory_order_relaxed);
   mScope.blocks.fetch_add(1, std::memory_order_relaxed);
}

// What the modulation of `module` is in band `band` after its smoothing, back in
// the shape's own 0..1 -- which is what the trace draws over the shape. Where an
// effect has no smoothing of its own, it is the shape.
float Engine::traceOf(int module, int band, float y) const {
   if (!moduleOn(module))
      return -1.0f;
   const BandState &s = mBands[band];
   switch (module) {
   case kModuleVolume: return std::sqrt(std::max(0.0f, s.smoothedMod));
   case kModuleFilter: return s.smoothedFilter < 0.0f ? y : s.smoothedFilter;
   case kModuleDrive:
      return mParams.drvDrive > 0.0f && s.smoothedDrive >= 0.0f
                ? clampf(s.smoothedDrive / mParams.drvDrive, 0.0f, 1.0f)
                : y;
   case kModuleCrush:
      return mParams.crsAmount > 0.0f ? clampf(s.smoothedCrush / mParams.crsAmount, 0.0f, 1.0f)
                                      : y;
   case kModuleTime: {
      const float range =
         clampf(mParams.timRangeSec, 0.001f, kMaxTimeRange) * static_cast<float>(mSampleRate);
      return clampf(1.0f - s.smoothedOffset / range, 0.0f, 1.0f);
   }
   case kModuleWidth: return clampf(0.5f * s.smoothedWidth, 0.0f, 1.0f);
   case kModulePan: return s.smoothedPan;
   default: return y;
   }
}

bool Engine::moduleOn(int module) const {
   switch (module) {
   case kModuleVolume: return mParams.volOn;
   case kModuleFilter: return mParams.fltOn;
   case kModuleDrive: return mParams.drvOn;
   case kModuleCrush: return mParams.crsOn;
   case kModuleTime: return mParams.timOn;
   case kModuleWidth: return mParams.widOn;
   case kModulePan: return mParams.panOn;
   default: return false;
   }
}

// One sample of one band through the chain, in the chain's order. The `y`
// values are the interpolated curve positions for this sample, by module; the
// filter's own position was turned into coefficients at the top of the
// control block.
void Engine::processBand(int band, float &l, float &r, const float *y, int type) {
   BandState &state = mBands[band];
   for (int i = 0; i < mOrderCount; ++i) {
      const int module = mOrder[i];
      if (!moduleOn(module))
         continue;
      switch (module) {
      case kModuleVolume: stageVolume(state, l, r, y[kModuleVolume]); break;
      case kModuleFilter: stageFilter(state, l, r, type); break;
      case kModuleDrive: stageDrive(state, l, r, y[kModuleDrive]); break;
      case kModuleCrush: stageCrush(state, l, r, y[kModuleCrush]); break;
      case kModuleTime: stageTime(state, l, r, y[kModuleTime]); break;
      case kModuleWidth: stageWidth(state, l, r, y[kModuleWidth]); break;
      case kModulePan: stagePan(state, l, r, y[kModulePan]); break;
      default: break;
      }
   }
}

void Engine::stageVolume(BandState &state, float &l, float &r, float yVol) {
   // Squared so that a curve drawn halfway down is roughly -12 dB, which is
   // where a sidechain duck sits in practice.
   const float target = yVol * yVol;
   state.smoothedMod += mVolCoef * (target - state.smoothedMod);
   const float g = state.smoothedMod * mParams.volTrimGain;
   l *= g;
   r *= g;
}

void Engine::stageFilter(BandState &state, float &l, float &r, int type) {
   if (mParams.fltDrive > 0.0f) {
      // A little saturation in front of the filter, which is what makes a
      // resonant sweep sound driven rather than clinical.
      const float d = 1.0f + mParams.fltDrive * 3.0f;
      l = std::tanh(l * d) / d;
      r = std::tanh(r * d) / d;
   }
   switch (type) {
   case kHighpass:
      l = highpassOf(state.filter[0], l);
      r = highpassOf(state.filter[1], r);
      break;
   case kBandpass:
      l = state.filter[0].bandpassNormalised(l);
      r = state.filter[1].bandpassNormalised(r);
      break;
   case kNotch:
      l = state.filter[0].notch(l);
      r = state.filter[1].notch(r);
      break;
   case kLowpass:
   default:
      l = state.filter[0].lowpass(l);
      r = state.filter[1].lowpass(r);
      break;
   }
}

void Engine::stageDrive(BandState &state, float &l, float &r, float yDrv) {
   const float target = clampf(mParams.drvDrive * yDrv, 0.0f, 1.0f);
   if (state.smoothedDrive < 0.0f)
      state.smoothedDrive = target;
   else
      state.smoothedDrive += mDrvCoef * (target - state.smoothedDrive);
   const float amount = state.smoothedDrive;
   const float pre = mParams.drvInputGain * dbToGainf(amount * 24.0f);
   const float beforeL = l, beforeR = r;
   float dl = driveShape(mParams.drvStyle, l * pre, amount);
   float dr = driveShape(mParams.drvStyle, r * pre, amount);

   // Rectifying leaves DC behind; nothing else here does, and running the
   // blocker on every style would cost the others a little bottom end.
   if (mParams.drvStyle == kRectify) {
      dl = state.dc[0].tick(dl);
      dr = state.dc[1].tick(dr);
   }

   // Level compensation: the louder the drive, the more the shaper would
   // otherwise raise the average. The same curve for every style on purpose:
   // a comparison between two styles should be a comparison of character.
   const float comp = 1.0f / (1.0f + 2.5f * amount);
   dl *= comp * mParams.drvOutputGain;
   dr *= comp * mParams.drvOutputGain;

   // Tone: a one-pole at 800 Hz, mixed in progressively. Positive is the
   // low-passed copy, negative the high-passed one.
   if (mParams.drvTone != 0.0f) {
      const float lpL = state.tone[0].tick(dl);
      const float lpR = state.tone[1].tick(dr);
      const float t = std::fabs(mParams.drvTone);
      if (mParams.drvTone > 0.0f) {
         dl = dl + (lpL - dl) * t;
         dr = dr + (lpR - dr) * t;
      } else {
         dl = dl + ((dl - lpL) - dl) * t;
         dr = dr + ((dr - lpR) - dr) * t;
      }
   }
   l = beforeL + (dl - beforeL) * mParams.drvMix;
   r = beforeR + (dr - beforeR) * mParams.drvMix;
}

void Engine::stageCrush(BandState &state, float &l, float &r, float yCrs) {
   const float target = clampf(mParams.crsAmount * yCrs, 0.0f, 1.0f);
   state.smoothedCrush += mCrsCoef * (target - state.smoothedCrush);
   const float amount = clampf(state.smoothedCrush, 0.0f, 1.0f);
   if (amount > 1.0e-4f) {
      const float bits = clampf(static_cast<float>(mParams.crsBits), 2.0f, 16.0f);
      const float levels = std::exp2(16.0f - (16.0f - bits) * amount) - 1.0f;
      const float down = 1.0f + (static_cast<float>(mParams.crsDown) - 1.0f) * amount;

      // Sample and hold, at a fractional rate. The phase starts at one so
      // the first sample is captured rather than a zero from the reset.
      state.holdPhase += 1.0f / down;
      if (state.holdPhase >= 1.0f) {
         state.holdPhase -= std::floor(state.holdPhase);
         state.hold[0] = l;
         state.hold[1] = r;
      }
      if (levels > 1.0f) {
         const float ql = std::round(state.hold[0] * levels) / levels;
         const float qr = std::round(state.hold[1] * levels) / levels;
         l += (ql - l) * mParams.crsMix;
         r += (qr - r) * mParams.crsMix;
      }
   }
}

void Engine::stageTime(BandState &state, float &l, float &r, float yTim) {
   const float rangeSamples =
      clampf(mParams.timRangeSec, 0.001f, kMaxTimeRange) * static_cast<float>(mSampleRate);
   // y = 1 is now, y = 0 is one range into the past. A curve falling at one
   // range per cycle therefore holds a moment still, and falling faster
   // plays it backwards -- the two gestures the stage exists for.
   const float target = (1.0f - yTim) * rangeSamples;
   state.smoothedOffset += mTimeCoef * (target - state.smoothedOffset);
   const float offset = clampf(state.smoothedOffset, 0.0f, rangeSamples);

   const int len = state.length;
   if (len > 0) {
      const int w = state.write;
      state.delay[0][w] = l;
      state.delay[1][w] = r;

      float pos = static_cast<float>(w) - offset;
      if (pos < 0.0f)
         pos += len;
      const int i0 = static_cast<int>(pos) % len;
      const int i1 = (i0 + 1) % len;
      const float f = pos - std::floor(pos);
      l = lerpf(state.delay[0][i0], state.delay[0][i1], f);
      r = lerpf(state.delay[1][i0], state.delay[1][i1], f);

      state.write = (w + 1) % len;
   }
}

void Engine::stageWidth(BandState &state, float &l, float &r, float yWid) {
   // 0 mono, 0.5 the plain image, 1 double width.
   const float target = clampf(2.0f * yWid, 0.0f, 2.0f);
   state.smoothedWidth += mWidCoef * (target - state.smoothedWidth);
   const float mid = 0.5f * (l + r);
   const float side = 0.5f * (l - r) * state.smoothedWidth;
   l = mid + side;
   r = mid - side;
}

void Engine::stagePan(BandState &state, float &l, float &r, float yPan) {
   // A balance rather than a constant-power pan: the centre is unity, so a
   // neutral curve is transparent, and the opposite side is what is turned
   // down as the shape leaves the middle.
   const float target = clampf(yPan, 0.0f, 1.0f);
   state.smoothedPan += mPanCoef * (target - state.smoothedPan);
   const float p = state.smoothedPan;
   if (p < 0.5f)
      r *= 2.0f * p;
   else
      l *= 2.0f * (1.0f - p);
}

} // namespace shapeau
