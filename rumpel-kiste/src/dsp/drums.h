#pragma once

// The eleven voices of the machine, from the voicing board's circuit diagram.
//
// Every voice is written as the circuit is drawn: oscillators, envelopes, the
// filters between them, and the voltage-controlled amplifiers that let them
// out. Component values become time constants and corner frequencies in
// DrumEngine::setParams(); the per-sample code only runs what that computed.
// The comments beside each voice name the parts.
//
// What the service notes do not give is not guessed at in here. Absolute
// oscillator frequencies, sweep depths and the content of the cymbal ROMs come
// in through DrumParams from parameters under ADVANCED, the Mods.
//
// The engine is deterministic: no clock, no global state, and every random
// number comes from a generator that reset() puts back to the same state. The
// same events at the same sample rate render the same samples, which is what
// lets a change be proved to have changed nothing.

#include <cmath>
#include <cstdint>

#include "plugincore/dsp/filters.h"
#include "plugincore/dsp/rng.h"

#include "dsp/drive.h"

namespace rumpelkiste {

using plugincore::OnePoleHp;
using plugincore::OnePoleLp;
using plugincore::RngLite;
using plugincore::Svf;

// Everything the voices are told, in real units. Filled from the parameter
// table by the plugin whenever a parameter moves.
struct DrumParams {
   // Bass drum.
   float bdTune = 0.5f;     // position of VR2, 0..1
   float bdLevel = 0.8f;    // 0..1
   float bdAttack = 0.5f;   // 0..1
   float bdDecaySec = 0.073f;

   // Snare.
   float sdTune = 0.5f;
   float sdLevel = 0.8f;
   float sdTone = 0.5f;
   float sdSnappy = 0.5f;

   // Toms, low, mid, hi.
   float tomTune[3] = {0.5f, 0.5f, 0.5f};
   float tomLevel[3] = {0.8f, 0.8f, 0.8f};
   float tomDecaySec[3] = {0.15f, 0.15f, 0.15f};

   float rsLevel = 0.7f;
   float cpLevel = 0.7f;

   // One hi-hat voice, two decays.
   float hhLevel = 0.7f;
   float chDecaySec = 0.033f;
   float ohDecaySec = 0.33f;

   float crLevel = 0.6f;
   float crTune = 0.5f;
   float rdLevel = 0.6f;
   float rdTune = 0.5f;

   // The master, as a linear gain.
   float gain = 0.5f;

   // The mods.
   float bdPitchHz = 50.0f;
   float bdSweep = 3.0f;    // start frequency over settle frequency, at full accent
   float bdShape = 0.5f;
   float sdPitchHz = 190.0f;
   float tomPitchHz = 90.0f;
   float tomSweep = 0.5f;
   float tomNoise = 0.5f;
   float rsGateSec = 0.005f;
   float cpSpreadSec = 0.009f;
   float hatColor = 0.5f;
   float cymColor = 0.5f;
   int dacBits = 6;

   // The drive bus. `driveMode` is a DriveMode; `route` says which voices go
   // through it when that is Selected.
   int driveMode = 0;
   int driveModel = 0;
   float drive = 0.35f;
   float driveBias = 0.0f;
   float driveToneHz = 12000.0f;
   float driveMix = 1.0f;
   bool route[11] = {false, false, false, false, false, false,
                     false, false, false, false, false};
};

// Rates and time constants the voices share, computed once per parameter
// change rather than per sample.
struct DrumCoefs {
   float sr = 48000.0f;
   float invSr = 1.0f / 48000.0f;
   int trigSamples = 96; // the machine's trigger pulse is 2 ms wide (p.5)

   // Bass drum.
   float bdPitchC9 = 0.0f; // e^-1/tau per sample, C9 through R57 + VR2
   float bdAmpAttack = 0.0f;
   float bdAmpDecay = 0.0f;
   int bdHold = 0;
   float bdClickDecay = 0.0f;
   float bdPulseDecay = 0.0f;
   float bdShapeK = 2.0f;
   float bdShapeNorm = 1.0f;
   float bdShapeOff = 0.0f;

   // Snare.
   float sdBend = 0.0f;   // IC36's supply, C66 through R248: 4.7 ms
   float sdEnv1 = 0.0f;   // ENV3, C83 through R291: 22 ms
   float sdEnv2 = 0.0f;   // ENV2, C82 through R295: 10.3 ms
   float sdTail = 0.0f;   // ENV4, C67 through R254 + VR7
   float sdSnap = 0.0f;   // ENV5 after the trigger ends

   // Toms.
   float tomFast = 0.0f;         // the fast part of the sweep, 30 ms
   float tomSlow = 0.0f;         // C16 through R60: 220 ms
   float tomEnv[3] = {0, 0, 0};  // the main VCA's envelope, from Decay
   float tomKnee[3] = {1, 1, 1}; // where that VCA runs out of headroom
   float tomKneeNorm[3] = {1, 1, 1};
   float tomLowEnv[3] = {0, 0, 0};
   float tomLowLag[3] = {0, 0, 0};
   float tomTop[3] = {0, 0, 0};
   float tomSquare = 0.0f;
   float tomNoise = 0.0f;
   float tomTick = 0.0f;         // C54 through R198: 0.47 ms

   // Rim shot.
   float rsExcite = 0.0f;
   float rsGate = 0.0f;

   // Hand clap.
   float cpBurst = 0.0f;
   float cpBurstFast = 0.0f;
   float cpTailDecay = 0.0f;
   float cpTailRise = 0.0f;
   int cpSpread = 432;
   long cpOnset[4] = {0, 0, 0, 0};

   // Master.
   float muteRamp = 0.0f;
};

// The noise generator every analog voice shares (p.6: "a quasi-random noise
// generator having two shift registers connected in cascade making up 32
// stages", two 4006s). One source for all of them, because on the machine it
// is one source: a snare and a clap on the same step hear the same noise.
//
// The shift register is clocked by the XOR oscillator round IC31, R185 (33 k)
// and C47 (100 pF), at about 1 / (2.2 R C) = 138 kHz -- far above any sample
// rate, so every output sample is the average of the two or three bits that
// went past during it, which is what the analog path after it sees as well.
class NoiseSource {
public:
   void reset() {
      mState = 0x1u; // D48 starts the register from one bit on power-up
      mPhase = 0.0;
   }
   void setRate(double sampleRate) {
      mInc = 138000.0 / (sampleRate > 1.0 ? sampleRate : 48000.0);
      mNorm = static_cast<float>(std::sqrt(mInc));
   }
   float tick();

private:
   uint32_t mState = 1u;
   double mPhase = 0.0;
   double mInc = 2.875;
   float mNorm = 1.7f;
};

// Six-bit sample playback, for the hi-hat and the two cymbals.
//
// On the machine these are recordings, stored compressed as six-bit PCM in
// ROMs (IC62, IC64, IC69) that the service notes do not reproduce. What this
// plays instead is a synthesised stand-in, made one ROM sample at a time at
// the ROM clock and put through everything the circuit puts the real data
// through: the latch, the six-bit converter, the hold between clock edges and
// the envelope that restores the decay the compression took out.
class RomVoice {
public:
   enum Kind { kHat = 0, kCrash, kRide };

   void init(Kind kind) { mKind = kind; }
   void reset();
   // `closed` only means something for the hat.
   void trigger(float accent, bool closed, float gain);
   void setRates(float sampleRate, float romRate, float color, int bits);
   void setDecay(float closedSec, float openSec, float sampleRate);
   void setLevel(float level) { mLevel = level; }
   float tick();
   bool active() const { return mActive; }
   bool closed() const { return mClosed; }

private:
   float romSample(int index);

   Kind mKind = kHat;
   bool mActive = false;
   bool mClosed = false;
   float mLevel = 0.7f;
   float mAccent = 1.0f;

   // The ROM run.
   double mPos = 0.0;
   double mInc = 0.625;
   int mNext = 0;
   int mLength = 32768;
   float mHeld = 0.0f;
   float mRomRate = 30000.0f;
   float mSampleRate = 48000.0f;
   float mStep = 1.0f / 31.0f; // one LSB of the converter
   float mColor = 0.5f;

   // The stand-in's oscillators and noise, advanced at the ROM clock and reset
   // on every trigger, so every hit reads the same "ROM" -- which is what a
   // recording does.
   double mPhase[6] = {0, 0, 0, 0, 0, 0};
   double mFreq[6] = {0, 0, 0, 0, 0, 0};
   double mBell[8] = {0, 0, 0, 0, 0, 0, 0, 0};
   double mBellFreq[8] = {0, 0, 0, 0, 0, 0, 0, 0};
   RngLite mRng;
   Svf mRomHp1, mRomHp2, mRomPeak, mRomPeak2, mRomLp;

   // After the converter.
   Svf mRecon;
   long mTime = 0;
   float mDecayCoef = 0.0f;
   float mClosedTau = 1.0f, mOpenTau = 1.0f;
   float mCharge = 0.0f;
};

class DrumEngine {
public:
   static constexpr int kMaxPending = 32;

   void prepare(double sampleRate);
   void reset();
   // The controls a sounding voice reads while it rings glide to what this
   // hands over rather than jumping to it; the rest -- switches, and what a
   // hit takes at its start -- take effect at once. The first call after
   // prepare() or reset() is not a change and is taken as it is.
   void setParams(const DrumParams &p);

   // One hit. `accent` is the accent voltage for it, 0 for a plain step and 1
   // for everything the plugin has to give, already combined from the total
   // and the local accent.
   //
   // `gain` scales the hit without changing what the accent does to it: what
   // the lighter first stroke of a flam is.
   void trigger(int voice, float accent, float gain = 1.0f);
   // The same, `delay` samples from now. What a flam's second stroke uses.
   void triggerLater(int voice, float accent, uint32_t delay, float gain = 1.0f);

   void setMuted(int voice, bool muted);

   // Writes `n` mono samples.
   void process(float *out, uint32_t n);

   bool isSilent() const;
   double tailSeconds() const { return 3.0; }
   uint32_t activeVoiceCount() const;

private:
   void startVoice(int voice, float accent, float gain);

   // Everything derived from `p`, which becomes mP. What setParams() did
   // before the controls glided.
   void derive(const DrumParams &p);
   // The part of derive() a glide step has to redo: the values the voices
   // read through a derived coefficient rather than straight from mP.
   void deriveLive();
   // One step of the glide from mP towards mTarget, over `frames` samples.
   void glideStep(uint32_t frames);

   // Every voice's own state, as the circuit has it.
   struct Bd {
      bool active = false;
      double phase = 0.0;
      float e1 = 0.0f; // C9, the pitch envelope
      float depth = 0.0f;
      float amp = 0.0f, amp2 = 0.0f, ampPeak = 0.0f;
      int trig = 0, hold = 0;
      float click = 0.0f, pulse = 0.0f;
      OnePoleLp noiseLp;
      OnePoleHp pulseHp;
   };
   struct Sd {
      bool active = false;
      double p1 = 0.0, p2 = 0.0;
      float bend = 0.0f;
      float env1 = 0.0f, env2 = 0.0f;
      float tail = 0.0f, snap = 0.0f;
      int trig = 0;
      Svf lp, hp;
   };
   struct Tom {
      bool active = false;
      // The three oscillators: C19's (the lowest), C18's (the one heard as
      // the pitch) and C20's.
      double pLow = 0.0, pMain = 0.0, pTop = 0.0;
      float fast = 0.0f, slow = 0.0f;
      float env = 0.0f, lowSrc = 0.0f, lowEnv = 0.0f, top = 0.0f, square = 0.0f;
      float noise = 0.0f, tick = 0.0f;
      float amp = 0.0f, noiseAmp = 0.0f;
      int trig = 0;
      Svf hp;
   };
   // One bridged-T resonator round its op-amp: the input plus a band-pass
   // with the network's gain, as a biquad.
   struct Resonator {
      float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
      float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
      void set(float hz, float q, float gain, float sr);
      float tick(float x) {
         const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
         x2 = x1;
         x1 = x;
         y2 = y1;
         y1 = y;
         return y;
      }
   };
   struct Rs {
      bool active = false;
      float excite = 0.0f, gate = 0.0f;
      long time = 0;
      Resonator f1, f2, f3;
   };
   struct Cp {
      bool active = false;
      long time = 0;
      float amp = 0.0f;
      float tailD = 0.0f, tailA = 0.0f;
      float burst = 0.0f, burstFast = 0.0f;
      int next = 0;
      Svf bp, bpWide;
      OnePoleHp hp, hp2;
      OnePoleLp lp;
   };

   float tickBd(float noise);
   float tickSd(float noise);
   float tickTom(int which, float noise);
   float tickRs();
   float tickCp(float noise);

   DrumParams mP;
   DrumCoefs mC;

   // What the controls last asked for. mP differs from it only while a glide
   // is under way.
   DrumParams mTarget;
   bool mPrimed = false;           // whether setParams() has been called since reset()
   uint32_t mGlideLeft = 0;        // samples left before mP is snapped to mTarget
   uint32_t mGlideTotal = 0;       // how many a glide takes, from the sample rate
   uint32_t mGlidePhase = 0;       // samples since the last glide step
   uint32_t mDriveGlideFrames = 0; // samples since the drive stage's settings last stepped
   float mGlideCoef = 1.0f;        // one glide step

   // What the ROM voices and the drive stage were last derived for. Both are
   // rederived on a glide step only when one of their inputs has moved: the
   // drive stage's level match costs up to 30 us on the heaviest model.
   float mRatesSet[4] = {-1.0f, -1.0f, -1.0f, -1.0f}; // crTune, rdTune, hatColor, cymColor
   int mDacBitsSet = -1;
   int mDriveModelSet = -1;
   float mDriveSet = -1.0f;
   float mDriveBiasSet = -2.0f;
   NoiseSource mNoise;

   Bd mBd;
   Sd mSd;
   Tom mTom[3];
   Rs mRs;
   Cp mCp;
   RomVoice mHat, mCrash, mRide;

   struct Pending {
      int voice;
      float accent;
      uint32_t delay;
      float gain;
   };
   Pending mPending[kMaxPending];
   int mPendingCount = 0;

   bool mMuted[11] = {false, false, false, false, false, false,
                      false, false, false, false, false};
   float mMuteGain[11] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
   OnePoleHp mDcBlock;
   // The drive bus: SäureKiste's stage and the one-pole Tone after it.
   DriveStage mDrive;
   OnePoleLp mDriveTone;
   float mSampleRate = 48000.0f;
};

} // namespace rumpelkiste
