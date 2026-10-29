#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "dsp/curve.h"
#include "plugincore/dsp/filters.h"

namespace shapeau {

// Everything the engine needs to know, already in real units: hertz, seconds,
// decibels converted to gain, enum values as their small integers. The plugin
// fills one of these from its parameter atomics once per block; nothing below
// knows what a parameter id is.
struct EngineParams {
   // Global.
   float inputGain = 1.0f;
   float outputGain = 1.0f;
   float mix = 1.0f; // 0 dry .. 1 wet
   float splitLowHz = 200.0f;
   float splitHighHz = 2000.0f;
   bool bandOn[kNumBands] = {true, true, true};

   // Volume stage. The curve is a gain shape: 0 is silence, 1 is unity, and the
   // curve is squared so that its middle sits near -12 dB, which is where a
   // ducking curve wants it.
   bool volOn = false;
   float volTrimGain = 1.0f;
   int volRate = kRate4;
   float volPhase = 0.0f;
   float volSmooth = 0.0f;

   // Filter stage. The curve is a cutoff position: 1 is the cutoff the knob
   // sets, 0 is that cutoff pushed down by the module's depth in octaves.
   bool fltOn = false;
   float fltCutHz = 1000.0f;
   float fltRes = 0.2f;
   int fltDepthOct = 5;
   float fltDrive = 0.0f;
   int fltType[kNumBands] = {kLowpass, kLowpass, kLowpass};
   int fltRate = kRate4;
   float fltPhase = 0.0f;
   float fltSmooth = 0.0f;

   // Drive stage. The curve scales the Drive knob: 1 is the amount set, 0 is
   // nothing driven at all.
   bool drvOn = false;
   int drvStyle = kSoftClip;
   float drvDrive = 0.4f;
   float drvInputGain = 1.0f;
   float drvOutputGain = 1.0f;
   float drvTone = 0.0f; // -1 high-pass .. +1 low-pass, around 800 Hz
   float drvMix = 1.0f;
   int drvRate = kRate4;
   float drvPhase = 0.0f;
   float drvSmooth = 0.0f;

   // Time stage. The curve is a position in the last `range` seconds: 1 is now,
   // 0 is `range` seconds ago. A curve that falls at the rate the buffer fills
   // holds one moment still; one that falls faster reads backwards.
   bool timOn = false;
   float timRangeSec = 0.5f;
   float timStep = 0.3f;
   int timRate = kRate4;
   float timPhase = 0.0f;
   float timSmooth = 0.0f;

   // Crush stage. The curve scales the amount: 1 is the full bit and
   // sample-rate reduction set below, 0 is clean.
   bool crsOn = false;
   float crsAmount = 0.6f;
   int crsBits = 6;
   int crsDown = 8;
   float crsMix = 1.0f;
   int crsRate = kRate4;
   float crsPhase = 0.0f;
   float crsSmooth = 0.0f;

   // Pan stage. The curve is a position: 0 hard left, 0.5 centre, 1 hard
   // right, so neutral is the middle rather than the top.
   bool panOn = false;
   int panRate = kRate4;
   float panPhase = 0.0f;
   float panSmooth = 0.15f;

   // Width stage. The curve maps 0 .. 1 onto 0 .. 2 x stereo width, so 0 is
   // mono, 0.5 the plain image and 1 double width.
   bool widOn = false;
   int widRate = kRate4;
   float widPhase = 0.0f;
   float widSmooth = 0.15f;
};

// The whole audio path: three Linkwitz-Riley bands, each through volume, filter,
// drive and time in that order, summed back together and mixed against the dry
// signal.
//
// One engine belongs to one plugin instance and is used by the audio thread
// only. Curves arrive through publishCurves() from the main thread and are
// picked up through a seqlock at the top of the next block, so the writer never
// waits for the audio thread and the audio thread never waits for the writer.
// What the editor's scope draws: the signal going into the rack and coming out
// of it, and what the selected effect's modulation actually did, laid out along
// one cycle of that effect's shape. The audio thread writes it bin by bin at
// the same phase the shape is read at, so the waveform under a point of the
// shape is the audio that point acted on; the window reads it whenever it
// paints. Every value is an atomic on its own -- a bin read halfway through
// being written is one bin drawn a block late, which nobody can see.
struct Scope {
   static constexpr int kBins = 480;
   std::atomic<float> inLo[kBins], inHi[kBins];   // input, the lowest and highest sample
   std::atomic<float> outLo[kBins], outHi[kBins]; // output, the same
   std::atomic<float> trace[kBins];               // the modulation, 0..1; < 0 is none
   std::atomic<float> playhead{-1.0f};            // where the shape is being read, 0..1
   std::atomic<uint32_t> blocks{0};               // counts blocks written, for repainting

   Scope() { clear(); }
   void clear() {
      for (int i = 0; i < kBins; ++i) {
         inLo[i].store(0.0f, std::memory_order_relaxed);
         inHi[i].store(0.0f, std::memory_order_relaxed);
         outLo[i].store(0.0f, std::memory_order_relaxed);
         outHi[i].store(0.0f, std::memory_order_relaxed);
         trace[i].store(-1.0f, std::memory_order_relaxed);
      }
      playhead.store(-1.0f, std::memory_order_relaxed);
   }
};

class Engine {
public:
   void prepare(double sampleRate, uint32_t maxFrames);
   void reset();
   void setParams(const EngineParams &params);
   void publishCurves(const CurveSet &set);

   // `beats` is the host's position in quarter notes at the top of this block;
   // a negative value means the transport is not running and the curves free-run
   // at `bpm` (or 120 if that is not positive either).
   void process(float *left, float *right, uint32_t frames, double beats, double bpm);

   // How long the time stage can keep sounding after its input stops: its own
   // look-back range, plus the release of the filters behind it.
   double tailSeconds() const;

   bool silent() const { return mSilent; }

   // The scope, and what it follows: the effect whose cycle it is laid out
   // along, and the band it listens to (`allBands`: the whole rack's input and
   // output, with the trace taken from `band`). Set from the main thread;
   // nothing is written while `active` is false, which is while no window is
   // open.
   const Scope &scope() const { return mScope; }
   void setScopeTarget(int module, int band, bool allBands, bool active) {
      mScopeModule.store(module, std::memory_order_relaxed);
      mScopeBand.store(band, std::memory_order_relaxed);
      mScopeAll.store(allBands, std::memory_order_relaxed);
      mScopeActive.store(active, std::memory_order_relaxed);
   }

private:
   struct BandState {
      plugincore::Svf filter[2]; // one per channel
      plugincore::OnePoleLp tone[2];
      plugincore::OnePoleHp dc[2];
      float smoothedMod = 1.0f;    // the volume curve, after smoothing
      float smoothedOffset = 0.0f; // the time read head, in samples
      float smoothedCrush = 0.0f;  // the crush curve, after smoothing
      // The drive amount and the filter's curve position, after smoothing. A
      // negative value is "nothing yet": the first value is taken as it is
      // rather than glided to from an arbitrary start.
      float smoothedDrive = -1.0f;
      float smoothedFilter = -1.0f;
      float holdPhase = 1.0f;      // position in the sample-and-hold cycle
      float hold[2] = {0.0f, 0.0f};
      float smoothedPan = 0.5f;   // 0 left, 0.5 centre, 1 right
      float smoothedWidth = 1.0f; // 0 mono, 1 plain, 2 double
      std::vector<float> delay[2];
      int write = 0;
      int length = 0;
   };

   void readCurves(CurveSet &into);
   void updateSplit();
   float phaseAt(int rate, float phaseOffset, uint32_t frame, double beats, double bpm) const;
   static float driveShape(int style, float x, float amount);
   static float smootherCoef(float smooth, double sampleRate);

   /// One sample of the chain for one band, in the chain's order.
   void processBand(int band, float &l, float &r, const float *y, int type);
   bool moduleOn(int module) const;

   // The stages, one sample each. `y` is the stage's curve position.
   void stageVolume(BandState &state, float &l, float &r, float yVol);
   void stageFilter(BandState &state, float &l, float &r, int type);
   void stageDrive(BandState &state, float &l, float &r, float yDrv);
   void stageCrush(BandState &state, float &l, float &r, float yCrs);
   void stageTime(BandState &state, float &l, float &r, float yTim);
   void stageWidth(BandState &state, float &l, float &r, float yWid);
   void stagePan(BandState &state, float &l, float &r, float yPan);

   double mSampleRate = 48000.0;
   EngineParams mParams;
   BandState mBands[kNumBands];

   // The band split. Two cascaded two-pole sections per side makes a
   // Linkwitz-Riley fourth order, whose lowpass and highpass sum back to flat
   // when they are in phase -- which is the whole reason for choosing it here.
   plugincore::Svf mSplitLowLp[2][2];
   plugincore::Svf mSplitLowHp[2][2];
   plugincore::Svf mSplitHighLp[2][2];
   plugincore::Svf mSplitHighHp[2][2];
   float mSplitLowCached = -1.0f, mSplitHighCached = -1.0f;

   // Smoothing coefficients, recomputed once per block.
   float mVolCoef = 1.0f, mDrvCoef = 1.0f, mTimeCoef = 1.0f, mCrsCoef = 1.0f, mFltCoef = 1.0f;
   float mPanCoef = 1.0f, mWidCoef = 1.0f;

   // Where a free-running curve has got to, in quarter notes, when the host
   // transport is stopped. Reset with the engine.
   double mFreeBeats = 0.0;

   // The published curve set, and the seqlock that hands it across.
   CurveSet mCurves;
   std::atomic<uint32_t> mCurveSeq{0};

   bool mSilent = true;

   // The scope's writer side. A target is read once per block; a change of it
   // clears the bins, because a waveform laid out along one effect's cycle is
   // meaningless along another's.
   void scopeBlock(uint32_t frames, double beats, double bpm);
   void scopeSample(uint32_t frame, float inL, float inR, float outL, float outR, float trace);
   void scopeEnd();
   float traceOf(int module, int band, float y) const;
   Scope mScope;
   std::atomic<int> mScopeModule{0}, mScopeBand{1};
   std::atomic<bool> mScopeAll{true}, mScopeActive{false};
   bool mScopeOn = false;     // this block
   int mScopeTarget = -1;     // the module this block's bins follow
   int mScopeTraceBand = 1;   // the band its trace and, unless all, its audio come from
   bool mScopeAllBands = true;
   int mScopeLastModule = -2, mScopeLastBand = -2;
   bool mScopeLastAll = false;
   double mScopePos0 = 0.0, mScopeInc = 0.0;
   int mScopeBin = -1;
   float mBinInLo = 0.0f, mBinInHi = 0.0f, mBinOutLo = 0.0f, mBinOutHi = 0.0f, mBinTrace = -1.0f;

   // The order the effects run in, worked out once per block from the chain
   // and the switches; see effectiveChain().
   int mOrder[kNumModules] = {};
   int mOrderCount = 0;
};

} // namespace shapeau
