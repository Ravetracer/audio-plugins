#include "drums.h"

#include <cmath>

#include "plugincore/dsp/fastmath.h"

#include "params.h"

namespace rumpelkiste {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

// e^(-1/tau) per sample: what an RC discharge multiplies by each sample.
float rc(float tauSec, float sr) {
   if (!(tauSec > 0.0f))
      return 0.0f;
   return std::exp(-1.0f / (tauSec * sr));
}

// The one-pole step towards a target with time constant tau.
float approach(float tauSec, float sr) { return 1.0f - rc(tauSec, sr); }

// The machine's pots. "A" is audio taper: about a tenth of the resistance at
// the middle of the travel, which is what a log pot measures.
float audioTaper(float x) {
   const float t = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
   return (std::pow(10.0f, 2.0f * t) - 1.0f) * (1.0f / 99.0f);
}

// A triangle from its phase, 0..1: zero at 0, +1 at a quarter, -1 at three
// quarters. Every oscillator on the board is a triangle core -- an integrator
// and a hysteresis comparator -- and every one of them starts from zero,
// because its integrator capacitor is shorted by a transistor on the trigger.
inline float tri(double p) {
   if (p < 0.25)
      return static_cast<float>(4.0 * p);
   if (p < 0.75)
      return static_cast<float>(2.0 - 4.0 * p);
   return static_cast<float>(4.0 * p - 4.0);
}

inline double wrap(double p) { return p >= 1.0 ? p - std::floor(p) : p; }

// The pair of diodes to ground that every one of those triangles meets on its
// way out (D10/D11, D65/D66, D20/D21 ...). They round the triangle's corners
// into something close to a sine; how close depends on how far past the
// diodes' knee the triangle swings.
inline float diodes(float x, float k, float norm) { return std::tanh(k * x) * norm; }

// A second-order section with a given Q from the shared state-variable filter,
// whose own control is a 0..1 resonance with k = 2 - 1.98 r and k = 1/Q.
void setQ(Svf &f, float hz, float q, float sr) {
   const float r = (2.0f - 1.0f / (q > 0.51f ? q : 0.51f)) / 1.98f;
   f.setCutoff(hz, r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r), sr);
}

inline float svfLp(Svf &f, float in) {
   float lp, bp, hp;
   f.tick(in, lp, bp, hp);
   return lp;
}
inline float svfHp(Svf &f, float in) {
   float lp, bp, hp;
   f.tick(in, lp, bp, hp);
   return hp;
}
inline float svfBp(Svf &f, float in) {
   float lp, bp, hp;
   f.tick(in, lp, bp, hp);
   return bp;
}

// The accent voltage as the voices see it. Every trigger carries some: the
// accent latches put a level out for an ordinary step too, and an accented one
// raises it.
inline float accLevel(float accent, float floor) { return floor + (1.0f - floor) * accent; }

// Output scaling per voice, so the eleven sit at the levels the machine's mix
// bus gives them relative to each other with every Level knob at the same
// place. Measured, not derived: see tools/analysis/README.md.
constexpr float kBdGain = 1.00f;
constexpr float kSdGain = 0.62f;
constexpr float kTomGain = 0.48f;
constexpr float kRsGain = 0.62f;
constexpr float kCpGain = 0.95f;
constexpr float kHatGain = 0.62f;
constexpr float kCymGain = 0.55f;

// The two voltage-controlled oscillators of each tom, relative to its lowest
// one, from the timing capacitors: C19/C18 and C19/C20 for the low tom, C33/C32
// and C33/C34 for the mid, C102/C97 and C102/C103 for the hi.
constexpr float kTomRatio1[3] = {0.033f / 0.022f, 0.027f / 0.018f, 0.022f / 0.015f};
constexpr float kTomRatio3[3] = {0.033f / 0.012f, 0.027f / 0.01f, 0.022f / 0.0082f};
// And each tom's lowest oscillator against the low tom's: C19, C33, C102.
constexpr float kTomScale[3] = {1.0f, 0.033f / 0.027f, 0.033f / 0.022f};

// The snare's two oscillators, C69 against C71.
constexpr float kSdRatio = 0.01f / 0.0068f;

} // namespace

// ---------------------------------------------------------------- the noise

float NoiseSource::tick() {
   mPhase += mInc;
   const int n = static_cast<int>(mPhase);
   mPhase -= n;
   if (n <= 0)
      return (mState & 1u) ? mNorm / 1.7f : -mNorm / 1.7f;
   int sum = 0;
   for (int i = 0; i < n; ++i) {
      // A maximal-length 32-bit register (x^32 + x^22 + x^2 + x + 1). The
      // machine's taps are not legible; the length is, and so is the promise
      // that it does not repeat within anything a person would hear.
      const uint32_t out = mState & 1u;
      mState >>= 1;
      if (out)
         mState ^= 0x80200003u;
      sum += out ? 1 : -1;
   }
   return static_cast<float>(sum) / static_cast<float>(n) * mNorm;
}

// ------------------------------------------------------------ the ROM voices

void RomVoice::reset() {
   mActive = false;
   mHeld = 0.0f;
   mRecon.reset();
   mRomHp1.reset();
   mRomHp2.reset();
   mRomPeak.reset();
}

void RomVoice::setRates(float sampleRate, float romRate, float color, int bits) {
   mSampleRate = sampleRate;
   mRomRate = romRate;
   mInc = static_cast<double>(romRate) / static_cast<double>(sampleRate);
   mColor = color;
   const int b = bits < 2 ? 2 : (bits > 16 ? 16 : bits);
   mStep = 1.0f / static_cast<float>((1 << (b - 1)) - 1);

   // The stand-in's own filters run at the ROM clock, exactly like the data
   // they stand in for, so retuning a cymbal transposes all of it.
   static const double kMetal[6] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
   static const double kBell[4] = {2950.0, 4150.0, 5230.0, 6840.0};
   if (mKind == kHat) {
      const double scale = 1.25 + 0.7 * color;
      for (int i = 0; i < 6; ++i)
         mFreq[i] = kMetal[i] * scale;
      const float hp = 5200.0f + 3600.0f * color;
      setQ(mRomHp1, hp, 0.7f, romRate);
      setQ(mRomHp2, hp, 0.9f, romRate);
      setQ(mRomPeak, 10200.0f + 2000.0f * color, 1.4f, romRate);
   } else if (mKind == kCrash) {
      const double scale = 0.8 + 0.5 * color;
      for (int i = 0; i < 6; ++i)
         mFreq[i] = kMetal[i] * scale;
      const float hp = 2400.0f + 2600.0f * color;
      setQ(mRomHp1, hp, 0.7f, romRate);
      setQ(mRomHp2, hp, 0.7f, romRate);
      setQ(mRomPeak, 6800.0f + 1800.0f * color, 1.0f, romRate);
   } else {
      const double scale = 0.85 + 0.3 * color;
      for (int i = 0; i < 6; ++i)
         mFreq[i] = kMetal[i] * scale * 1.4;
      for (int i = 0; i < 4; ++i)
         mBellFreq[i] = kBell[i] * scale;
      const float hp = 5200.0f + 2200.0f * color;
      setQ(mRomHp1, hp, 0.7f, romRate);
      setQ(mRomHp2, hp, 0.7f, romRate);
      setQ(mRomPeak, 9000.0f, 1.2f, romRate);
   }
   // What smooths the converter's steps on the way to the VCA. Its corner is
   // not legible; half the ROM clock is where a reconstruction filter goes,
   // and it stops short of the host's own Nyquist.
   const float recon = std::min(0.42f * romRate, 0.45f * sampleRate);
   setQ(mRecon, recon, 0.7f, sampleRate);
}

void RomVoice::setDecay(float closedSec, float openSec, float sampleRate) {
   mClosedTau = closedSec;
   mOpenTau = openSec;
   mDecayCoef = approach(mClosed ? mClosedTau : mOpenTau, sampleRate);
}

void RomVoice::trigger(float accent, bool closed, float gain) {
   mActive = true;
   mClosed = closed;
   mAccent = accLevel(accent, 0.5f) * gain;
   mPos = 0.0;
   mNext = 0;
   mHeld = 0.0f;
   mTime = 0;
   mCharge = 0.0f;
   // The address range each run reads (p.5's address table): the open hat
   // plays from the top of the ROM to 110 0000 0000 0000, 24576 samples; the
   // closed hat plays the last 8192. The cymbal ROMs are 32 kB each.
   if (mKind == kHat)
      mLength = closed ? 8192 : 24576;
   else
      mLength = 32768;
   mDecayCoef = approach(mClosed ? mClosedTau : mOpenTau, mSampleRate);
   for (int i = 0; i < 6; ++i)
      mPhase[i] = 0.0;
   for (int i = 0; i < 4; ++i)
      mBell[i] = 0.0;
   // The same "recording" every time: a fixed seed per ROM, and the closed hat
   // reads a different part of the hat ROM than the open one.
   mRng.seed(mKind == kHat ? (closed ? 0x51A9C0DEu : 0x0B5E55EDu)
                           : (mKind == kCrash ? 0xC4A5C0DEu : 0x71DEB0B0u));
   mRomHp1.reset();
   mRomHp2.reset();
   mRomPeak.reset();
}

float RomVoice::romSample(int index) {
   float metal = 0.0f;
   float ring = 0.0f;
   float sq[6];
   for (int i = 0; i < 6; ++i) {
      mPhase[i] += mFreq[i] / mRomRate;
      if (mPhase[i] >= 1.0)
         mPhase[i] -= 1.0;
      sq[i] = mPhase[i] < 0.5 ? 1.0f : -1.0f;
      metal += sq[i];
   }
   metal *= 1.0f / 6.0f;
   ring = (sq[0] * sq[1] + sq[2] * sq[3] + sq[4] * sq[5]) * (1.0f / 3.0f);
   const float noise = mRng.white();
   const float t = static_cast<float>(index);

   float v = 0.0f;
   if (mKind == kHat) {
      const float raw = 0.55f * metal + 0.5f * noise;
      const float hp = svfHp(mRomHp2, svfHp(mRomHp1, raw));
      const float peak = svfBp(mRomPeak, raw);
      // The recording's own decay, most of it compressed away before the data
      // was stored (p.5); the circuit puts the rest back.
      const float natural = mClosed ? 0.30f + 0.70f * std::exp(-t / 2500.0f)
                                    : 0.35f + 0.65f * std::exp(-t / 12000.0f);
      v = std::tanh(2.6f * (hp + 0.35f * peak) * natural);
   } else if (mKind == kCrash) {
      const float raw = 0.55f * ring + 0.55f * noise;
      const float hp = svfHp(mRomHp2, svfHp(mRomHp1, raw));
      const float peak = svfBp(mRomPeak, raw);
      const float natural = 0.5f + 0.5f * std::exp(-t / 20000.0f);
      v = std::tanh(2.2f * (hp + 0.3f * peak) * natural);
   } else {
      float bell = 0.0f;
      static const float kAmp[4] = {1.0f, 0.7f, 0.55f, 0.4f};
      static const float kLife[4] = {9000.0f, 6000.0f, 5000.0f, 3500.0f};
      for (int i = 0; i < 4; ++i) {
         mBell[i] += mBellFreq[i] / mRomRate;
         if (mBell[i] >= 1.0)
            mBell[i] -= 1.0;
         bell += kAmp[i] * std::exp(-t / kLife[i]) *
                 static_cast<float>(std::sin(kTwoPi * mBell[i]));
      }
      const float wash = svfHp(mRomHp2, svfHp(mRomHp1, 0.45f * noise + 0.25f * metal));
      v = std::tanh(1.8f * (0.6f * bell + wash + 0.15f * svfBp(mRomPeak, noise)));
   }
   return v;
}

float RomVoice::tick() {
   if (!mActive)
      return 0.0f;
   mPos += mInc;
   // Every ROM clock edge that went past during this host sample latches a new
   // value into the converter (IC68 for the hats), which then holds until the
   // next one: the staircase, and the images of the ROM clock above it, are
   // part of the sound.
   while (mNext < mLength && static_cast<double>(mNext) <= mPos) {
      const float v = romSample(mNext);
      mHeld = std::round(v / mStep) * mStep;
      ++mNext;
   }
   if (mNext >= mLength && mPos >= static_cast<double>(mLength))
      mHeld = 0.0f; // IC72a stops the counter at the end of the run

   const float y = svfLp(mRecon, mHeld);

   float gain;
   if (mKind == kHat) {
      // The anti-log converter (IC67a, Q84) turns C135's charge into a gain
      // that falls linearly in decibels: sixty of them by the time it is full.
      mCharge += (1.0f - mCharge) * mDecayCoef;
      gain = std::exp(-6.9078f * mCharge);
   } else {
      // The cymbals' envelope is the ROM address itself, run through a
      // resistor array and anti-log tapered (RA11, IC52b, Q70 for the crash):
      // it follows the read position, so a retuned cymbal decays faster or
      // slower along with its pitch.
      const float u = static_cast<float>(mPos / mLength);
      const float db = mKind == kCrash ? -44.0f * u
                                        : -14.0f * (1.0f - std::exp(-static_cast<float>(mPos) /
                                                                   1200.0f)) -
                                             30.0f * u;
      gain = std::exp(db * 0.11512925f);
   }
   ++mTime;
   if (gain < 1.0e-4f || (mNext >= mLength && mTime > static_cast<long>(mSampleRate * 0.01f) +
                                                        static_cast<long>(mLength / mInc)))
      mActive = false;
   return y * gain * mAccent * mLevel;
}

// ------------------------------------------------------------------ engine

void DrumEngine::prepare(double sampleRate) {
   mSampleRate = static_cast<float>(sampleRate > 1.0 ? sampleRate : 48000.0);
   mNoise.setRate(mSampleRate);
   mHat.init(RomVoice::kHat);
   mCrash.init(RomVoice::kCrash);
   mRide.init(RomVoice::kRide);
   mDcBlock.setCutoff(5.0f, mSampleRate);
   mDrive.prepare(mSampleRate);
   setParams(mP);
   reset();
}

void DrumEngine::reset() {
   mNoise.reset();
   mBd = Bd();
   mSd = Sd();
   for (Tom &t : mTom)
      t = Tom();
   mRs = Rs();
   mCp = Cp();
   mHat.reset();
   mCrash.reset();
   mRide.reset();
   mPendingCount = 0;
   mDcBlock.reset();
   mDrive.reset();
   mDriveTone.reset();
   // The filter coefficients live in the voices' own filter objects, which
   // the assignments above just threw away.
   setParams(mP);
}

void DrumEngine::setParams(const DrumParams &p) {
   mP = p;
   const float sr = mSampleRate;
   DrumCoefs &c = mC;
   c.sr = sr;
   c.invSr = 1.0f / sr;
   c.trigSamples = static_cast<int>(0.002f * sr + 0.5f);

   // Bass drum. C9 (0.33 uF) discharges through R57 (22 k) and VR2 (100 k A),
   // C1 (0.068 uF) through R12 (100 k).
   c.bdPitchC9 = rc(0.33e-6f * (22.0e3f + 100.0e3f * audioTaper(p.bdTune)), sr);
   c.bdPitchC1 = rc(0.068e-6f * 100.0e3f, sr);
   // C8 (0.33 uF) charges through R35 (1.8 k) for as long as the trigger lasts.
   c.bdAmpAttack = approach(0.33e-6f * 1.8e3f, sr);
   c.bdAmpDecay = rc(p.bdDecaySec, sr);
   // ENV2: C12 (0.033 uF) through R41 (22 k). The pulse: C5 (0.0068 uF)
   // through R14 (22 k).
   c.bdClickDecay = rc(0.033e-6f * 22.0e3f, sr);
   c.bdPulseDecay = rc(0.0068e-6f * 22.0e3f, sr);
   c.bdShapeK = 0.8f + 3.2f * p.bdShape;
   c.bdShapeNorm = 1.0f / std::tanh(c.bdShapeK);
   // R45 (4.7 k) and C13 (0.1 uF) on the noise into the click: 339 Hz. C11
   // (0.47 uF) into R8 || R44 couples the pulse: 49 Hz.
   mBd.noiseLp.setCutoff(339.0f, sr);
   mBd.pulseHp.setCutoff(49.0f, sr);

   // Snare. The bend is IC36's supply recovering through C66 (0.01 uF) and
   // R248 (470 k); the two oscillators' envelopes are C83 (0.1 uF) and C82
   // (0.047 uF) through 220 k each; the noise tail is C67 (0.47 uF) through
   // R254 (100 k) and VR7 (500 k B).
   c.sdBend = rc(0.01e-6f * 470.0e3f, sr);
   c.sdEnv1 = rc(0.1e-6f * 220.0e3f, sr);
   c.sdEnv2 = rc(0.047e-6f * 220.0e3f, sr);
   c.sdTail = rc(0.47e-6f * (100.0e3f + 500.0e3f * p.sdTone), sr);
   // ENV5 is the accent gated by the trigger through Q41, smoothed by C73
   // (470 pF) and R280 (220 k): the burst lasts as long as the trigger does.
   c.sdSnap = rc(470.0e-12f * 220.0e3f, sr);
   // IC40a: a unity-gain Sallen-Key low-pass, R300 = R301 = 22 k, C81 3300 pF,
   // C84 220 pF -- 8.5 kHz with a Q of 1.94. IC39a: a high-pass, C75 = C76 =
   // 2200 pF, R277 47 k, R276 6.8 k -- 4.05 kHz, Q 1.31.
   setQ(mSd.lp, 8490.0f, 1.94f, sr);
   setQ(mSd.hp, 4050.0f, 1.31f, sr);

   // Toms. The pitch envelopes are C17 (0.047 uF, R61 470 k) and C16 (0.1 uF,
   // R60 2.2 M); the main envelope is C23 (0.68 uF) through R111 and VR11; the
   // top oscillator's is C25 (0.056 uF) through R110 (470 k).
   c.tomFast = rc(0.047e-6f * 470.0e3f, sr);
   c.tomSlow = rc(0.1e-6f * 2.2e6f, sr);
   c.tomEnv3 = rc(0.056e-6f * 470.0e3f, sr);
   for (int i = 0; i < 3; ++i) {
      c.tomEnv2[i] = rc(p.tomDecaySec[i], sr);
      // The middle oscillator's VCA (Q19) follows the main envelope through
      // C22 (0.22 uF), which shortens it.
      c.tomEnv1[i] = rc(0.35f * p.tomDecaySec[i] + 0.005f, sr);
      // The tom noise: a Sallen-Key high-pass, C51 = C52 = 4700 pF, R191 10 k,
      // R192 220 k -- 722 Hz, Q 2.35.
      setQ(mTom[i].hp, 722.0f, 2.35f, sr);
   }
   // C54 (0.0047 uF after the serial-number change) through R198 (100 k).
   c.tomTick = rc(0.0047e-6f * 100.0e3f, sr);

   // Rim shot. The excitation is the trigger differentiated by C111 (0.018 uF)
   // into R395 || R396 (11 k). The resonators are bridged-T networks round
   // IC48a, IC49a and IC49b: f = 1 / (2 pi C sqrt(R r)), Q = sqrt(R / r) / 2.
   c.rsExcite = rc(0.018e-6f * 11.0e3f, sr);
   c.rsGate = rc(p.rsGateSec, sr);
   setQ(mRs.f1, 494.9f, 7.31f, sr);  // C112/C113 0.01 uF, R407 470 k, R394 2.2 k
   setQ(mRs.f2, 218.8f, 6.12f, sr);  // C115/C116 0.027 uF, R414 330 k, R411 2.2 k
   setQ(mRs.f3, 1053.0f, 7.31f, sr); // C117/C118 0.0047 uF, R416 470 k, R404 2.2 k
   // IC50b: a Sallen-Key high-pass, C121 = C122 = 0.01 uF, R420 220 k, R419
   // 4.7 k -- 495 Hz, Q 3.42.
   setQ(mRs.hp, 495.0f, 3.42f, sr);

   // Hand clap. IC26b is a multiple-feedback band-pass: R208 47 k in, R209
   // 10 k to ground, R207 150 k back, C42 = C43 = 4700 pF -- 963 Hz, Q 2.13,
   // gain 1.6. The claps go on through C56 and R202 (185 Hz); the tail through
   // R204 and C57 (339 Hz) and a VCA whose envelope is C59 through R233 (47 ms).
   setQ(mCp.bp, 963.0f, 2.13f, sr);
   mCp.hp.setCutoff(185.0f, sr);
   mCp.lp.setCutoff(339.0f, sr);
   c.cpSpread = static_cast<int>(p.cpSpreadSec * sr + 0.5f);
   if (c.cpSpread < 8)
      c.cpSpread = 8;
   c.cpBurst = rc(0.22f * p.cpSpreadSec, sr);
   c.cpLast = rc(1.3f * p.cpSpreadSec, sr);
   c.cpTailDecay = rc(0.1e-6f * 470.0e3f, sr);
   // How fast the tail comes up is not legible; the printed waveform shows the
   // four bursts standing clear of it, so it rises under them rather than
   // arriving with the first.
   c.cpTailRise = rc(0.012f, sr);

   // The ROM voices. The hat ROM is read at "about 60 kHz divided by two"
   // (p.5); the cymbals' clocks follow their Tune knobs, a fifth either way.
   const float crRate = 30000.0f * std::pow(2.0f, (p.crTune - 0.5f) * (14.0f / 12.0f));
   const float rdRate = 30000.0f * std::pow(2.0f, (p.rdTune - 0.5f) * (14.0f / 12.0f));
   mHat.setRates(sr, 30000.0f, p.hatColor, p.dacBits);
   mCrash.setRates(sr, crRate, p.cymColor, p.dacBits);
   mRide.setRates(sr, rdRate, p.cymColor, p.dacBits);
   mHat.setDecay(p.chDecaySec, p.ohDecaySec, sr);
   mHat.setLevel(p.hhLevel * kHatGain);
   mCrash.setLevel(p.crLevel * kCymGain);
   mRide.setLevel(p.rdLevel * kCymGain);

   c.muteRamp = approach(0.003f, sr);

   mDrive.setParams(p.driveModel, p.drive, p.driveBias, p.driveMix);
   mDriveTone.setCutoff(std::min(p.driveToneHz, sr * 0.45f), sr);
}

void DrumEngine::setMuted(int voice, bool muted) {
   if (voice >= 0 && voice < kNumVoices)
      mMuted[voice] = muted;
}

void DrumEngine::trigger(int voice, float accent, float gain) { startVoice(voice, accent, gain); }

void DrumEngine::triggerLater(int voice, float accent, uint32_t delay, float gain) {
   if (delay == 0) {
      startVoice(voice, accent, gain);
      return;
   }
   if (mPendingCount >= kMaxPending)
      return;
   mPending[mPendingCount++] = {voice, accent, delay, gain};
}

void DrumEngine::startVoice(int voice, float accent, float gain) {
   const float a = accent < 0.0f ? 0.0f : (accent > 1.0f ? 1.0f : accent);
   const float g = gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain);
   switch (voice) {
   case kVoiceBD: {
      Bd &b = mBd;
      b.active = true;
      b.phase = 0.0;
      b.e1 = 1.0f;
      b.e2 = 1.0f;
      // ACCENT reaches the pitch envelope through D5 and R34, so an accented
      // kick sweeps from higher up as well as landing harder.
      b.depth = (mP.bdSweep - 1.0f) * (0.7f + 0.3f * a);
      b.ampPeak = accLevel(a, 0.38f) * g;
      b.trig = mC.trigSamples;
      b.click = accLevel(a, 0.55f) * g;
      b.pulse = 1.0f;
      break;
   }
   case kVoiceSD: {
      Sd &s = mSd;
      s.active = true;
      s.p1 = 0.0;
      s.p2 = 0.0;
      s.bend = 1.0f;
      s.env1 = accLevel(a, 0.4f) * g;
      s.env2 = accLevel(a, 0.4f) * 0.9f * g;
      s.tail = accLevel(a, 0.5f) * g;
      s.snap = accLevel(a, 0.25f) * g;
      s.trig = mC.trigSamples;
      break;
   }
   case kVoiceLT:
   case kVoiceMT:
   case kVoiceHT: {
      Tom &t = mTom[voice - kVoiceLT];
      t.active = true;
      t.p1 = t.p2 = t.p3 = 0.0;
      t.fast = 1.0f;
      t.slow = 1.0f;
      const float amp = accLevel(a, 0.4f) * g;
      t.env1 = amp;
      t.env2 = amp;
      t.env3 = amp;
      t.tick = accLevel(a, 0.5f) * g;
      break;
   }
   case kVoiceRS:
      // The clamp diodes flatten the resonators whatever drives them, so an
      // accent reaches the rim shot's level where it reaches the gate: the
      // accent line feeds Q64, whose envelope opens Q65 after the diodes.
      mRs.active = true;
      mRs.excite = accLevel(a, 0.7f);
      mRs.gate = accLevel(a, 0.5f) * g;
      mRs.time = 0;
      break;
   case kVoiceCP:
      mCp.active = true;
      mCp.time = 0;
      mCp.amp = accLevel(a, 0.5f) * g;
      mCp.burst = 1.0f;
      mCp.tailD = 1.0f;
      mCp.tailA = 1.0f;
      break;
   case kVoiceCH:
      mHat.trigger(a, true, g);
      break;
   case kVoiceOH:
      mHat.trigger(a, false, g);
      break;
   case kVoiceCR:
      mCrash.trigger(a, false, g);
      break;
   case kVoiceRD:
      mRide.trigger(a, false, g);
      break;
   default:
      break;
   }
}

float DrumEngine::tickBd(float noise) {
   Bd &b = mBd;
   if (!b.active)
      return 0.0f;
   const DrumCoefs &c = mC;
   // The oscillator's charging current is the sum of R27's bias and the two
   // envelope capacitors' voltages, so the frequency is linear in them.
   const float f = mP.bdPitchHz * (1.0f + b.depth * (0.75f * b.e1 + 0.25f * b.e2));
   b.e1 *= c.bdPitchC9;
   b.e2 *= c.bdPitchC1;
   b.phase = wrap(b.phase + f * c.invSr);
   const float body = diodes(tri(b.phase), c.bdShapeK, c.bdShapeNorm);

   if (b.trig > 0) {
      --b.trig;
      b.amp += (b.ampPeak - b.amp) * c.bdAmpAttack;
   } else {
      b.amp *= c.bdAmpDecay;
   }

   // The ATTACK path: the pulse, and the noise low-passed by R45 and C13, both
   // through Q6 with ENV2.
   const float pulse = b.pulseHp.tick(b.pulse);
   b.pulse *= c.bdPulseDecay;
   const float lpNoise = b.noiseLp.tick(noise);
   const float click = b.click * mP.bdAttack * (-1.5f * pulse + 2.8f * lpNoise);
   b.click *= c.bdClickDecay;

   if (b.trig <= 0 && b.amp < 1.0e-5f && b.click < 1.0e-5f)
      b.active = false;
   return (body * b.amp + click) * mP.bdLevel * kBdGain;
}

float DrumEngine::tickSd(float noise) {
   Sd &s = mSd;
   if (!s.active)
      return 0.0f;
   const DrumCoefs &c = mC;
   // IC36's supply jumps from 2 V to 5 V on the trigger and falls back: the
   // buffers' swing, and with it both oscillators' frequency, follows it
   // (p.6's figure).
   const float bend = 1.0f + 1.5f * s.bend;
   s.bend *= c.sdBend;
   const float f1 = mP.sdPitchHz * (2.0f / 3.0f) * (1.0f + mP.sdTune) * bend;
   s.p1 = wrap(s.p1 + f1 * c.invSr);
   s.p2 = wrap(s.p2 + f1 * kSdRatio * c.invSr);
   // D65/D66 and D69/D70: "0.6 V" sines, the diagram says.
   const float y1 = diodes(tri(s.p1), 2.4f, 1.0f / std::tanh(2.4f)) * s.env1;
   const float y2 = diodes(tri(s.p2), 2.4f, 1.0f / std::tanh(2.4f)) * s.env2;
   s.env1 *= c.sdEnv1;
   s.env2 *= c.sdEnv2;

   const float lp = svfLp(s.lp, noise);
   const float hp = svfHp(s.hp, lp);
   const float tail = lp * s.tail;
   s.tail *= c.sdTail;
   const float snap = hp * s.snap;
   if (s.trig > 0)
      --s.trig;
   else
      s.snap *= c.sdSnap;

   if (s.trig <= 0 && s.env1 < 1.0e-5f && s.env2 < 1.0e-5f && s.tail < 1.0e-5f)
      s.active = false;
   const float noisePart = mP.sdSnappy * (0.55f * snap + 0.42f * tail);
   return (y1 + 0.8f * y2 + noisePart) * mP.sdLevel * kSdGain;
}

float DrumEngine::tickTom(int which, float noise) {
   Tom &t = mTom[which];
   if (!t.active)
      return 0.0f;
   const DrumCoefs &c = mC;
   const float sweep = 1.2f * mP.tomSweep;
   const float m = 1.0f + sweep * (0.8f * t.fast + 0.2f * t.slow);
   t.fast *= c.tomFast;
   t.slow *= c.tomSlow;
   const float f = mP.tomPitchHz * (2.0f / 3.0f) * (1.0f + mP.tomTune[which]) * kTomScale[which] * m;
   t.p2 = wrap(t.p2 + f * c.invSr);
   t.p1 = wrap(t.p1 + f * kTomRatio1[which] * c.invSr);
   t.p3 = wrap(t.p3 + f * kTomRatio3[which] * c.invSr);
   const float k = 2.4f, norm = 1.0f / std::tanh(2.4f);
   const float y = diodes(tri(t.p2), k, norm) * t.env2 + 0.55f * diodes(tri(t.p1), k, norm) * t.env1 +
                   0.35f * diodes(tri(t.p3), k, norm) * t.env3;
   t.env1 *= c.tomEnv1[which];
   t.env2 *= c.tomEnv2[which];
   t.env3 *= c.tomEnv3;
   const float nz = svfHp(t.hp, noise) * t.tick;
   t.tick *= c.tomTick;
   if (t.env2 < 1.0e-5f && t.env1 < 1.0e-5f && t.env3 < 1.0e-5f)
      t.active = false;
   return (y + 1.5f * mP.tomNoise * nz) * mP.tomLevel[which] * kTomGain;
}

float DrumEngine::tickRs() {
   Rs &r = mRs;
   if (!r.active)
      return 0.0f;
   const DrumCoefs &c = mC;
   const float e = r.excite;
   r.excite *= c.rsExcite;
   // The three resonators, summed through R408, R415 and R417 (12 k, 12 k,
   // 3.3 k) onto the node D91 and D92 clamp.
   const float sum = svfBp(r.f1, e) + svfBp(r.f2, e) + 3.64f * svfBp(r.f3, e);
   const float clipped = std::tanh(6.0f * sum);
   const float gated = clipped * r.gate;
   r.gate *= c.rsGate;
   const float out = svfHp(r.hp, gated);
   ++r.time;
   if (r.gate < 1.0e-6f && r.time > static_cast<long>(0.05f * c.sr))
      r.active = false;
   return out * mP.rsLevel * kRsGain;
}

float DrumEngine::tickCp(float noise) {
   Cp &p = mCp;
   if (!p.active)
      return 0.0f;
   const DrumCoefs &c = mC;
   const float bp = svfBp(p.bp, noise) * (1.0f / 2.13f) * 1.6f;
   // The sawtooth envelope: four bursts, each cut off by the next, the last
   // left to decay.
   const long spread = c.cpSpread;
   const long k = p.time / spread;
   if (k <= 3 && p.time % spread == 0)
      p.burst = 1.0f;
   const float claps = p.hp.tick(bp) * p.burst;
   p.burst *= k < 3 ? c.cpBurst : c.cpLast;
   // The tail rises under the bursts and outlasts them.
   const float tailEnv = (p.tailD - p.tailA) * 2.15f;
   p.tailD *= c.cpTailDecay;
   p.tailA *= c.cpTailRise;
   const float tail = p.lp.tick(bp) * tailEnv * 2.6f;
   ++p.time;
   if (k >= 3 && p.burst < 1.0e-5f && p.tailD < 1.0e-5f)
      p.active = false;
   return (claps + 0.45f * tail) * p.amp * mP.cpLevel * kCpGain;
}

void DrumEngine::process(float *out, uint32_t n) {
   const float ramp = mC.muteRamp;
   for (uint32_t i = 0; i < n; ++i) {
      // Flams and anything else that was scheduled.
      for (int k = 0; k < mPendingCount;) {
         if (mPending[k].delay == 0) {
            startVoice(mPending[k].voice, mPending[k].accent, mPending[k].gain);
            mPending[k] = mPending[--mPendingCount];
            continue;
         }
         --mPending[k].delay;
         ++k;
      }

      for (int v = 0; v < kNumVoices; ++v) {
         const float target = mMuted[v] ? 0.0f : 1.0f;
         mMuteGain[v] += (target - mMuteGain[v]) * ramp;
         if (std::fabs(target - mMuteGain[v]) < 1.0e-4f)
            mMuteGain[v] = target;
      }

      const float noise = mNoise.tick();
      // Each voice onto the dry bus or the drive bus.
      float v[kNumVoices];
      v[kVoiceBD] = tickBd(noise) * mMuteGain[kVoiceBD];
      v[kVoiceSD] = tickSd(noise) * mMuteGain[kVoiceSD];
      v[kVoiceLT] = tickTom(0, noise) * mMuteGain[kVoiceLT];
      v[kVoiceMT] = tickTom(1, noise) * mMuteGain[kVoiceMT];
      v[kVoiceHT] = tickTom(2, noise) * mMuteGain[kVoiceHT];
      v[kVoiceRS] = tickRs() * mMuteGain[kVoiceRS];
      v[kVoiceCP] = tickCp(noise) * mMuteGain[kVoiceCP];
      // One hat voice: whichever of the two rows last played it owns its mute
      // and its route.
      const int hatRow = mHat.closed() ? kVoiceCH : kVoiceOH;
      v[kVoiceCH] = 0.0f;
      v[kVoiceOH] = 0.0f;
      v[hatRow] = mHat.tick() * mMuteGain[hatRow];
      v[kVoiceCR] = mCrash.tick() * mMuteGain[kVoiceCR];
      v[kVoiceRD] = mRide.tick() * mMuteGain[kVoiceRD];

      float dry = 0.0f, wet = 0.0f;
      if (mP.driveMode == kDriveMaster) {
         for (int k = 0; k < kNumVoices; ++k)
            wet += v[k];
      } else if (mP.driveMode == kDriveSelected) {
         for (int k = 0; k < kNumVoices; ++k)
            (mP.route[k] ? wet : dry) += v[k];
      } else {
         for (int k = 0; k < kNumVoices; ++k)
            dry += v[k];
      }
      // The stage runs whenever the bus is in use, fed silence or not, so its
      // filters never restart with a click when a voice is routed to it.
      const float s = mP.driveMode == kDriveOff ? dry : dry + mDriveTone.tick(mDrive.tick(wet));

      out[i] = plugincore::softClip(mDcBlock.tick(s) * mP.gain);
   }
}

bool DrumEngine::isSilent() const {
   if (mPendingCount > 0)
      return false;
   if (mBd.active || mSd.active || mRs.active || mCp.active)
      return false;
   for (const Tom &t : mTom)
      if (t.active)
         return false;
   return !mHat.active() && !mCrash.active() && !mRide.active();
}

uint32_t DrumEngine::activeVoiceCount() const {
   uint32_t n = 0;
   n += mBd.active ? 1 : 0;
   n += mSd.active ? 1 : 0;
   for (const Tom &t : mTom)
      n += t.active ? 1 : 0;
   n += mRs.active ? 1 : 0;
   n += mCp.active ? 1 : 0;
   n += mHat.active() ? 1 : 0;
   n += mCrash.active() ? 1 : 0;
   n += mRide.active() ? 1 : 0;
   return n;
}

} // namespace rumpelkiste
