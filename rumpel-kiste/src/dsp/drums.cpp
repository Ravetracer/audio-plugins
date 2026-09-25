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

// Output scaling per voice, so the eleven sit at the levels a machine's
// outputs give them relative to each other with every Level knob at the same
// place: RMS over the first 150 ms of each against the bass drum's, measured
// from recordings (tools/analysis/README.md). The open hat's recording starts
// quieter than the closed hat's, and the ride's than the crash's.
constexpr float kBdGain = 0.41f;
constexpr float kSdGain = 0.58f;
constexpr float kTomGain = 0.36f;
constexpr float kRsGain = 0.77f;
constexpr float kCpGain = 1.22f;
constexpr float kHatGain = 0.40f;
constexpr float kOhLevel = 0.46f;
constexpr float kCymGain = 0.20f;
constexpr float kRideLevel = 0.42f;

// The three voltage-controlled oscillators of each tom, from the timing
// capacitors: C19/C18 and C19/C20 for the low tom, C33/C32 and C33/C34 for the
// mid, C102/C97 and C102/C103 for the hi. The one the ear takes for the pitch
// is the middle one, C18's; C19's sits a fifth under it.
constexpr float kTomRatio1[3] = {0.033f / 0.022f, 0.027f / 0.018f, 0.022f / 0.015f};
constexpr float kTomRatio3[3] = {0.033f / 0.012f, 0.027f / 0.01f, 0.022f / 0.0082f};
// And each tom against the low tom: C19, C33, C102.
constexpr float kTomScale[3] = {1.0f, 0.033f / 0.027f, 0.033f / 0.022f};

// What the toms do that the diagram does not say, measured from recordings of
// a machine (tools/analysis/README.md, "Measured").
//
// The sweep is current added to the Tune current, so it is a number of hertz on
// top of wherever Tune puts the tom rather than a ratio. At the default Tom
// Sweep it starts at these fractions of the tom's pitch at Tune's centre.
constexpr float kTomSweepFast = 0.26f; // decaying in 30 ms
constexpr float kTomSweepSlow = 0.12f; // decaying in C16/R60's 220 ms
// The main VCA's decay at the top of the Decay knob. The three toms share the
// part values and still differ, by these.
constexpr float kTomDecayMax[3] = {0.114f, 0.079f, 0.084f};
// C19's oscillator comes in behind the other two -- its VCA follows the main
// envelope through a lag -- and then outlasts them a little.
constexpr float kTomLowLevel[3] = {0.97f, 1.18f, 1.13f};
constexpr float kTomLowLag[3] = {0.035f, 0.020f, 0.025f};
constexpr float kTomLowLonger = 1.08f;
// C20's oscillator: short, and a third of the level or more at the hit.
constexpr float kTomTopLevel[3] = {0.66f, 0.75f, 0.66f};
constexpr float kTomTopDecay[3] = {0.027f, 0.029f, 0.026f};
// The comparator's square wave that leaks out of the main oscillator for the
// first few cycles, and gives the hit its edge.
constexpr float kTomSquare = 0.23f;
constexpr float kTomSquareDecay = 0.013f;
// The noise under the hit: longer than C54's tick and independent of Decay.
constexpr float kTomNoiseDecay = 0.020f;
constexpr float kTomNoiseWash = 0.05f;
constexpr float kTomNoiseTick = 0.2f;

// Decay's pot position as a fraction of its travel, from the time constant the
// knob shows (C23 through R111 and VR11, 38-378 ms).
inline float tomDecayTravel(float tauSec) {
   const float x = (tauSec - 0.038f) / 0.340f;
   return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

// The rim shot's three resonators as measured: within a few per cent of the
// frequencies the parts give, a little less sharp than they give.
constexpr float kRsF1 = 494.9f * 0.983f, kRsQ1 = 7.31f * 0.92f;
constexpr float kRsF2 = 218.8f * 1.042f, kRsQ2 = 6.12f * 0.97f;
constexpr float kRsF3 = 1053.0f * 0.963f, kRsQ3 = 7.31f * 0.78f;
// The excitation's decay, and where D91/D92 start to clamp against it.
constexpr float kRsExcite = 0.00048f;
constexpr float kRsClamp = 0.39f;
// Q65 passes one polarity 1.58 times as strongly as the other.
constexpr float kRsAsym = 1.58f;

// What the bass drum does that the diagram does not say, measured from
// recordings of a machine (tools/analysis/README.md, "Measured").
//
// The sweep is one exponential, whatever C1 adds to it, with a time constant
// Tune sets: at Tune 0, 25, 50, 75 and 100 %.
constexpr float kBdSweepTau[5] = {0.0076f, 0.0097f, 0.0118f, 0.0145f, 0.0289f};
// The kick holds its level for this long from the hit, then falls in two
// stages, (1 + t/tau) e^(-t/tau), with tau set by Decay at 0, 25 ... 100 %.
constexpr float kBdHold = 0.045f;
constexpr float kBdDecayTau[5] = {0.016f, 0.025f, 0.032f, 0.042f, 0.064f};
// D10/D11 do not round the triangle symmetrically: the offset that gives the
// recordings' second harmonic.
constexpr float kBdShapeBias = -0.05f;
// The click's level against Attack's travel.
constexpr float kBdClickFloor = 0.35f;
constexpr float kBdClickRange = 0.57f;

// The clap, measured: where its four bursts land, in units of Clap Spread
// (the machine's gaps are 10.0, 11.5 and 10.3 ms), how long the tail lasts
// (C59 and R233 give 47 ms), how loud it is under the bursts, and the air the
// bursts carry above what IC26b's band-pass leaves.
constexpr float kCpOnset[4] = {0.0f, 0.943f, 2.028f, 3.0f};
constexpr float kCpTailDecay = 0.105f;
constexpr float kCpTail = 0.2f;
constexpr float kCpBurstDecay = 0.0045f;
constexpr float kCpBurstQ = 1.25f;
constexpr float kCpBurstHp = 400.0f;
constexpr float kCpBurstFast = 1.13f;
constexpr float kCpBurstFastDecay = 0.0006f;

// The hats, measured: the ROM clock (the converter's null sits at 31.5 kHz;
// p.5 says "about 60 kHz, divided by two"), the open hat's run (16384 samples,
// 0.52 s -- the address table read as 24576), the VCA's time constant at
// CH Decay 0, 25 ... 100 %, and the open hat's at the top of OH Decay. The
// closed hat's recording fades out on its own after 85 ms, 0.3 dB/ms faster.
constexpr float kHatClock = 31500.0f;
// The stand-in hat's filters, fitted to the recordings' spectrum at Hat
// Color 50 %: one high-pass and a broad peak, weighted, and the reconstruction
// filter at 0.6 of the ROM clock.
constexpr float kHatHp = 3000.0f;
constexpr float kHatPeak = 6500.0f;
constexpr float kHatPeakQ = 1.0f;
constexpr float kHatPeakLevel = 1.5f;
constexpr float kRomRecon = 0.6f;

// The cymbals, measured: the ROM clock the Tune knobs set, linear in hertz
// from the converter's null (28.5 kHz, 37 kHz and 45.8 kHz at 0, 50 and
// 100 %), the envelope over the ROM's run -- it falls faster towards the end,
// and the ride drops 5 dB at the hit as its ping goes -- and the stand-ins'
// content, fitted to the recordings' spectra at the centre clock: the ride's
// bell partials where the recording has them.
constexpr float kCymClock = 37000.0f;
constexpr float kCymClockLow = 28500.0f, kCymClockHigh = 45800.0f;
constexpr double kRideBell[8] = {4016.0, 4069.0, 4610.0, 6728.0, 7276.0, 7614.0, 8155.0, 9927.0};
constexpr float kRideBellLevel[8] = {0.55f, 0.7f, 0.3f, 0.9f, 0.8f, 1.0f, 1.0f, 1.0f};
// The stand-ins' filters and levels, fitted to the recordings' spectra in the
// first 100 ms and 300-600 ms at Cym Color 50 %.
constexpr float kCrHp = 480.0f, kCrLp = 11000.0f, kCrLpQ = 1.6f;
constexpr float kCrPeak1 = 700.0f, kCrPeak1Q = 1.44f, kCrPeak1Level = 0.3f;
constexpr float kCrPeak2 = 5000.0f, kCrPeak2Q = 4.1f, kCrPeak2Level = 0.48f;
constexpr float kCrRing = 0.8f, kCrDrive = 0.324f;
constexpr float kRdHp = 768.0f, kRdLp = 14000.0f;
constexpr float kRdPeak = 202.5f, kRdPeakQ = 6.8f, kRdPeakLevel = 0.245f;
constexpr float kRdMetal = 0.225f, kRdBell = 0.5f, kRdBellLife = 38400.0f, kRdDrive = 5.1f;
constexpr float kRdPing = 2.0f, kRdPingHp = 9000.0f, kRdPingLife = 3000.0f;
constexpr int kOhLength = 16384;
constexpr float kChTau[5] = {0.0087f, 0.0255f, 0.0331f, 0.0393f, 0.0445f};
constexpr float kOhTauMax = 0.120f;
constexpr float kChFadeStart = 0.085f;
constexpr float kChFade = 0.3f; // dB per ms

// A curve through five measurements at 0, 25, 50, 75 and 100 %.
inline float table5(const float (&t)[5], float x) {
   const float u = (x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x)) * 4.0f;
   const int i = u >= 4.0f ? 3 : static_cast<int>(u);
   return t[i] + (t[i + 1] - t[i]) * (u - static_cast<float>(i));
}

// VR5's position from the time constant the Decay knob shows: C8 through R58
// (47 k) and VR5 (1 M audio taper).
inline float bdDecayTravel(float tauSec) {
   const float r = (tauSec / 0.33e-6f - 47.0e3f) / 1.0e6f;
   const float x = 0.5f * std::log10(1.0f + 99.0f * (r > 0.0f ? r : 0.0f));
   return x > 1.0f ? 1.0f : x;
}

// The snare's two oscillators. C69 against C71 gives 1.47; the recordings sit
// at 1.50 at every Tune setting, and that is what is used.
constexpr float kSdRatio = 1.5f;
// Measured from the recordings: the oscillators' envelopes (C83 and C82 give
// 22 and 10.3 ms), the upper one's level, and the noise tail's time constant
// at Tone 0, 25 ... 100 % (C67 and VR7 give 47 to 282 ms).
constexpr float kSdEnv1 = 0.027f;
constexpr float kSdEnv2 = 0.025f;
constexpr float kSdUpper = 0.49f;
constexpr float kSdTailTau[5] = {0.024f, 0.042f, 0.057f, 0.075f, 0.077f};

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
   // The stand-in is computed as if at the clock it was designed for and
   // played at the clock the Tune knob sets, so retuning a cymbal transposes
   // all of it, as replaying a recording faster does.
   mRomRate = mKind == kHat ? romRate : kCymClock;
   mInc = static_cast<double>(romRate) / static_cast<double>(sampleRate);
   mColor = color;
   const int b = bits < 2 ? 2 : (bits > 16 ? 16 : bits);
   mStep = 1.0f / static_cast<float>((1 << (b - 1)) - 1);
   const float design = mRomRate;

   static const double kMetal[6] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
   const float c = 0.6f + 0.8f * color;
   if (mKind == kHat) {
      const double scale = 1.25 + 0.7 * color;
      for (int i = 0; i < 6; ++i)
         mFreq[i] = kMetal[i] * scale;
      setQ(mRomHp1, kHatHp * c, 0.7f, design);
      setQ(mRomHp2, 20.0f, 0.7f, design);
      setQ(mRomPeak, kHatPeak * c, kHatPeakQ, design);
   } else if (mKind == kCrash) {
      const double scale = 0.8 + 0.5 * color;
      for (int i = 0; i < 6; ++i)
         mFreq[i] = kMetal[i] * scale;
      setQ(mRomHp1, kCrHp * c, 0.7f, design);
      setQ(mRomLp, std::min(kCrLp * c, 0.45f * design), kCrLpQ, design);
      setQ(mRomPeak, kCrPeak1 * c, kCrPeak1Q, design);
      setQ(mRomPeak2, kCrPeak2 * c, kCrPeak2Q, design);
   } else {
      const double scale = 0.85 + 0.3 * color;
      for (int i = 0; i < 6; ++i)
         mFreq[i] = kMetal[i] * scale * 1.4;
      for (int i = 0; i < 8; ++i)
         mBellFreq[i] = kRideBell[i] * (scale / 1.0);
      setQ(mRomHp1, kRdHp * c, 0.7f, design);
      setQ(mRomLp, std::min(kRdLp * c, 0.45f * design), 0.7f, design);
      setQ(mRomPeak, kRdPeak * c, kRdPeakQ, design);
      setQ(mRomPeak2, kRdPingHp * c, 0.7f, design);
   }
   // What smooths the converter's steps on the way to the VCA. Its corner is
   // not legible; the recordings' top octave puts it above half the ROM
   // clock, and it stops short of the host's own Nyquist.
   const float recon = std::min(kRomRecon * romRate, 0.45f * sampleRate);
   setQ(mRecon, recon, 0.7f, sampleRate);
}

void RomVoice::setDecay(float closedSec, float openSec, float sampleRate) {
   // The knobs show C135's charging time through R451 + VR21 (10-110 ms) and
   // R452 + VR23 (100-1100 ms); what the VCA does at each position of the pot
   // is measured. The open hat's curve is the closed hat's, scaled.
   const float xc = (closedSec - 0.010f) / 0.100f, xo = (openSec - 0.100f) / 1.000f;
   mClosedTau = table5(kChTau, xc);
   mOpenTau = table5(kChTau, xo) * (kOhTauMax / kChTau[4]);
   mDecayCoef = rc(mClosed ? mClosedTau : mOpenTau, sampleRate);
}

void RomVoice::trigger(float accent, bool closed, float gain) {
   mActive = true;
   mClosed = closed;
   // Accent lifts the hats 2.3 times from a plain step and the cymbals 2.45
   // times, as recorded.
   mAccent = accLevel(accent, mKind == kHat ? 0.44f : 0.41f) * gain;
   if (mKind == kHat && !closed)
      mAccent *= kOhLevel;
   mPos = 0.0;
   mNext = 0;
   mHeld = 0.0f;
   mTime = 0;
   mCharge = 1.0f;
   // The address range each run reads (p.5's address table): the open hat
   // plays from the top of the ROM to 110 0000 0000 0000, 24576 samples; the
   // closed hat plays the last 8192. The cymbal ROMs are 32 kB each.
   if (mKind == kHat)
      mLength = closed ? 8192 : kOhLength;
   else
      mLength = 32768;
   mDecayCoef = rc(mClosed ? mClosedTau : mOpenTau, mSampleRate);
   for (int i = 0; i < 6; ++i)
      mPhase[i] = 0.0;
   for (int i = 0; i < 8; ++i)
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
      // The recording's own decay was compressed away before the data was
      // stored (p.5), all but the closed hat's fade after 85 ms; the circuit
      // puts the rest back.
      const float fadeFrom = kChFadeStart * mRomRate;
      const float natural =
         mClosed && t > fadeFrom ? std::exp(-kChFade * 0.11512925f * (t - fadeFrom) * 1000.0f / mRomRate) : 1.0f;
      v = std::tanh(2.6f * (hp + kHatPeakLevel * peak) * natural);
   } else if (mKind == kCrash) {
      const float raw = kCrRing * ring + noise;
      const float body = svfHp(mRomHp1, svfLp(mRomLp, raw));
      const float v1 = svfBp(mRomPeak, raw), v2 = svfBp(mRomPeak2, raw);
      // Lightly driven, and scaled to fill the converter as a recording would.
      v = std::tanh(kCrDrive * (body + kCrPeak1Level * v1 + kCrPeak2Level * v2)) / std::tanh(kCrDrive * 2.0f);
   } else {
      float bell = 0.0f;
      for (int i = 0; i < 8; ++i) {
         mBell[i] += mBellFreq[i] / mRomRate;
         if (mBell[i] >= 1.0)
            mBell[i] -= 1.0;
         bell += kRideBellLevel[i] * std::exp(-t / kRdBellLife) * static_cast<float>(std::sin(kTwoPi * mBell[i]));
      }
      const float wash = svfHp(mRomHp1, svfLp(mRomLp, noise + kRdMetal * metal));
      const float ping = svfHp(mRomPeak2, noise) * std::exp(-t / kRdPingLife);
      v = std::tanh(kRdDrive * (kRdBell * bell + wash + kRdPeakLevel * svfBp(mRomPeak, noise) + kRdPing * ping));
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
      // that falls linearly in decibels: a plain exponential, as measured.
      gain = mCharge;
      mCharge *= mDecayCoef;
   } else {
      // The cymbals' envelope is the ROM address itself, run through a
      // resistor array and anti-log tapered (RA11, IC52b, Q70 for the crash):
      // it follows the read position, so a retuned cymbal decays faster or
      // slower along with its pitch.
      const float u = static_cast<float>(mPos / mLength);
      const float u4 = u * u * u * u;
      float db = -20.0f * u - 25.0f * u4;
      if (mKind == kRide)
         db -= 5.0f * (1.0f - std::exp(-static_cast<float>(mPos) / (0.040f * kCymClock)));
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

   // Bass drum. C9 (0.33 uF) discharges through R57 (22 k) and VR2 (100 k A):
   // 7.3 ms to 40 ms by the parts; the recordings give 7.6 ms to 29 ms.
   c.bdPitchC9 = rc(table5(kBdSweepTau, p.bdTune), sr);
   // C8 (0.33 uF) charges through R35 (1.8 k) for as long as the trigger lasts.
   c.bdAmpAttack = approach(0.33e-6f * 1.8e3f, sr);
   // Decay shows C8's time constant through R58 and VR5, 15.5-345 ms; what is
   // heard is the measured hold and fall at that position of the pot.
   c.bdAmpDecay = rc(table5(kBdDecayTau, bdDecayTravel(p.bdDecaySec)), sr);
   c.bdHold = static_cast<int>(kBdHold * sr + 0.5f);
   // ENV2: C12 (0.033 uF) through R41 (22 k). The pulse: C5 (0.0068 uF)
   // through R14 (22 k).
   c.bdClickDecay = rc(0.033e-6f * 22.0e3f, sr);
   c.bdPulseDecay = rc(0.0068e-6f * 22.0e3f, sr);
   c.bdShapeK = 1.8f * std::pow(2.2f, 2.0f * p.bdShape - 1.0f);
   c.bdShapeOff = std::tanh(c.bdShapeK * kBdShapeBias);
   c.bdShapeNorm = 1.0f / (c.bdShapeOff - std::tanh(c.bdShapeK * (kBdShapeBias - 1.0f)));
   // R45 (4.7 k) and C13 (0.1 uF) on the noise into the click: 339 Hz. C11
   // (0.47 uF) into R8 || R44 couples the pulse: 49 Hz.
   mBd.noiseLp.setCutoff(339.0f, sr);
   mBd.pulseHp.setCutoff(49.0f, sr);

   // Snare. The bend is IC36's supply recovering through C66 (0.01 uF) and
   // R248 (470 k). The two oscillators' envelopes (C83 and C82 through 220 k)
   // and the noise tail (C67 through R254 and VR7, 500 k B) are measured.
   c.sdBend = rc(0.01e-6f * 470.0e3f, sr);
   c.sdEnv1 = rc(kSdEnv1, sr);
   c.sdEnv2 = rc(kSdEnv2, sr);
   c.sdTail = rc(table5(kSdTailTau, p.sdTone), sr);
   // ENV5 is the accent gated by the trigger through Q41, smoothed by C73
   // (470 pF) and R280 (220 k): the burst lasts as long as the trigger does.
   c.sdSnap = rc(470.0e-12f * 220.0e3f, sr);
   // IC40a: a unity-gain Sallen-Key low-pass, R300 = R301 = 22 k, C81 3300 pF,
   // C84 220 pF -- 8.5 kHz with a Q of 1.94. IC39a: a high-pass, C75 = C76 =
   // 2200 pF, R277 47 k, R276 6.8 k -- 4.05 kHz, Q 1.31.
   setQ(mSd.lp, 8490.0f, 1.94f, sr);
   setQ(mSd.hp, 4050.0f, 1.31f, sr);

   // Toms. The sweep's slow part is C16 (0.1 uF) through R60 (2.2 M); its fast
   // part and everything about the VCAs are measured (see the constants).
   c.tomFast = rc(0.030f, sr);
   c.tomSlow = rc(0.1e-6f * 2.2e6f, sr);
   for (int i = 0; i < 3; ++i) {
      // Decay moves the main VCA's time constant over less than the 10:1 its
      // parts suggest: about 2.5:1, on a curve.
      const float x = tomDecayTravel(p.tomDecaySec[i]);
      const float q = 0.4f + 0.8f * x - 0.2f * x * x;
      const float tau = kTomDecayMax[i] * q;
      c.tomEnv[i] = rc(tau, sr);
      // The VCA saturates at the top of the envelope, so the hit holds before
      // it falls; the knee comes at the same time for every setting.
      c.tomKnee[i] = std::exp(0.73f / q);
      c.tomKneeNorm[i] = 1.0f / std::tanh(c.tomKnee[i]);
      c.tomLowEnv[i] = rc(kTomLowLonger * tau, sr);
      c.tomLowLag[i] = approach(kTomLowLag[i], sr);
      c.tomTop[i] = rc(kTomTopDecay[i], sr);
      // The tom noise: a Sallen-Key high-pass, C51 = C52 = 4700 pF, R191 10 k,
      // R192 220 k -- 722 Hz, Q 2.35.
      setQ(mTom[i].hp, 722.0f, 2.35f, sr);
   }
   c.tomSquare = rc(kTomSquareDecay, sr);
   c.tomNoise = rc(kTomNoiseDecay, sr);
   // C54 (0.0047 uF after the serial-number change) through R198 (100 k): the
   // tick when the trigger ends.
   c.tomTick = rc(0.0047e-6f * 100.0e3f, sr);

   // Rim shot. The excitation is the trigger differentiated by C111; the
   // resonators are bridged-T networks round IC48a, IC49a and IC49b, each
   // passing its input plus a band-pass with a gain of R / 2r: R407/R394,
   // R414/R411 and R416/R404.
   c.rsExcite = rc(kRsExcite, sr);
   // Q65's envelope: C119 (0.047 uF) into R403 (1 M) by default.
   c.rsGate = rc(p.rsGateSec, sr);
   mRs.f1.set(kRsF1, kRsQ1, 470.0f / (2.0f * 2.2f), sr);
   mRs.f2.set(kRsF2, kRsQ2, 330.0f / (2.0f * 2.2f), sr);
   mRs.f3.set(kRsF3, kRsQ3, 470.0f / (2.0f * 2.2f), sr);

   // Hand clap. IC26b is a multiple-feedback band-pass: R208 47 k in, R209
   // 10 k to ground, R207 150 k back, C42 = C43 = 4700 pF -- 963 Hz, Q 2.13,
   // gain 1.6. The claps go on through C56 and R202 (185 Hz); the tail through
   // R204 and C57 (339 Hz) and a VCA whose envelope is C59 through R233 (47 ms).
   setQ(mCp.bp, 963.0f, 2.13f, sr);
   setQ(mCp.bpWide, 963.0f, kCpBurstQ, sr);
   mCp.hp.setCutoff(kCpBurstHp, sr);
   mCp.hp2.setCutoff(kCpBurstHp, sr);
   mCp.lp.setCutoff(339.0f, sr);
   c.cpSpread = static_cast<int>(p.cpSpreadSec * sr + 0.5f);
   if (c.cpSpread < 8)
      c.cpSpread = 8;
   for (int i = 0; i < 4; ++i)
      c.cpOnset[i] = static_cast<long>(kCpOnset[i] * p.cpSpreadSec * sr + 0.5f);
   c.cpBurst = rc(kCpBurstDecay, sr);
   c.cpBurstFast = rc(kCpBurstFastDecay, sr);
   c.cpTailDecay = rc(kCpTailDecay, sr);
   // The recordings have the tail come in with the last burst, standing clear
   // of the three before it, as the printed waveform does.
   c.cpTailRise = rc(0.001f, sr);

   // The ROM voices. The hat ROM is read at "about 60 kHz divided by two"
   // (p.5); the cymbals' clocks follow their Tune knobs, a fifth either way.
   const float crRate = kCymClockLow + (kCymClockHigh - kCymClockLow) * p.crTune;
   const float rdRate = kCymClockLow + (kCymClockHigh - kCymClockLow) * p.rdTune;
   mHat.setRates(sr, kHatClock, p.hatColor, p.dacBits);
   mCrash.setRates(sr, crRate, p.cymColor, p.dacBits);
   mRide.setRates(sr, rdRate, p.cymColor, p.dacBits);
   mHat.setDecay(p.chDecaySec, p.ohDecaySec, sr);
   mHat.setLevel(p.hhLevel * kHatGain);
   mCrash.setLevel(p.crLevel * kCymGain);
   mRide.setLevel(p.rdLevel * kCymGain * kRideLevel);

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
      // The recordings sweep from the same height at every accent; accent
      // only lands the kick harder, twice as hard at the top.
      b.depth = mP.bdSweep - 1.0f;
      b.ampPeak = accLevel(a, 0.5f) * g;
      b.trig = mC.trigSamples;
      b.hold = mC.bdHold;
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
      // Accent doubles the snare's drum and lifts its noise far less, 1.4
      // times, as recorded.
      s.env1 = accLevel(a, 0.45f) * g;
      s.env2 = accLevel(a, 0.45f) * 0.9f * g;
      s.tail = accLevel(a, 0.7f) * g;
      s.snap = accLevel(a, 0.7f) * g;
      s.trig = mC.trigSamples;
      break;
   }
   case kVoiceLT:
   case kVoiceMT:
   case kVoiceHT: {
      Tom &t = mTom[voice - kVoiceLT];
      t.active = true;
      t.pLow = t.pMain = t.pTop = 0.0;
      t.fast = 1.0f;
      t.slow = 1.0f;
      t.env = 1.0f;
      t.lowSrc = 1.0f;
      t.lowEnv = 0.0f;
      t.top = 1.0f;
      t.square = 1.0f;
      t.noise = 1.0f;
      t.tick = 0.0f;
      t.trig = mC.trigSamples;
      // Accent lifts the tones 2.3 times from a plain step and the noise more.
      t.amp = accLevel(a, 0.43f) * g;
      t.noiseAmp = accLevel(a, 0.27f) * g;
      break;
   }
   case kVoiceRS:
      // Accent reaches the rim shot where it reaches Q65, after the diodes:
      // the recordings keep one shape at every accent, 1.6 times louder at
      // the top.
      mRs.active = true;
      mRs.excite = 1.0f;
      mRs.gate = accLevel(a, 0.64f) * g;
      mRs.time = 0;
      break;
   case kVoiceCP:
      mCp.active = true;
      mCp.time = 0;
      mCp.amp = accLevel(a, 0.56f) * g;
      mCp.burst = 0.0f;
      mCp.burstFast = 0.0f;
      mCp.next = 0;
      mCp.tailD = 0.0f;
      mCp.tailA = 0.0f;
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
   // The oscillator's charging current is R27's bias plus the envelope's
   // voltage, so the frequency is linear in it.
   const float f = mP.bdPitchHz * (1.0f + b.depth * b.e1);
   b.e1 *= c.bdPitchC9;
   b.phase = wrap(b.phase + f * c.invSr);
   // The first half-cycle is negative, as recorded.
   const float body = -(std::tanh(c.bdShapeK * (tri(b.phase) + kBdShapeBias)) - c.bdShapeOff) * c.bdShapeNorm;

   if (b.trig > 0) {
      --b.trig;
      b.amp += (b.ampPeak - b.amp) * c.bdAmpAttack;
      b.amp2 = b.amp;
   } else if (b.hold > 0) {
      b.amp2 = b.amp;
   } else {
      b.amp *= c.bdAmpDecay;
      b.amp2 += (b.amp - b.amp2) * (1.0f - c.bdAmpDecay);
   }
   if (b.hold > 0)
      --b.hold;

   // The ATTACK path: the pulse, and the noise low-passed by R45 and C13, both
   // through Q6 with ENV2.
   const float pulse = b.pulseHp.tick(b.pulse);
   b.pulse *= c.bdPulseDecay;
   const float lpNoise = b.noiseLp.tick(noise);
   // The recordings click even with Attack fully down, and the knob does most
   // of its work in the top half of its travel.
   const float attack = kBdClickFloor + kBdClickRange * mP.bdAttack * mP.bdAttack;
   const float click = b.click * attack * (-1.5f * pulse + 2.8f * lpNoise);
   b.click *= c.bdClickDecay;

   if (b.trig <= 0 && b.hold <= 0 && b.amp2 < 1.0e-5f && b.click < 1.0e-5f)
      b.active = false;
   return (body * b.amp2 + click) * mP.bdLevel * kBdGain;
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
   const float noisePart = mP.sdSnappy * (0.48f * snap + 0.37f * tail);
   // The first half-cycle is negative, as recorded.
   return -(y1 + kSdUpper * y2 + noisePart) * mP.sdLevel * kSdGain;
}

float DrumEngine::tickTom(int which, float noise) {
   Tom &t = mTom[which];
   if (!t.active)
      return 0.0f;
   const DrumCoefs &c = mC;
   // The pitch the ear takes: C18's oscillator. Tune's divider spans an octave
   // round the centre; the sweep adds a number of hertz on top.
   const float centre = mP.tomPitchHz * kTomScale[which];
   const float sweep = 2.0f * mP.tomSweep;
   const float f = centre * ((2.0f / 3.0f) * (1.0f + mP.tomTune[which]) +
                             sweep * (kTomSweepFast * t.fast + kTomSweepSlow * t.slow));
   t.fast *= c.tomFast;
   t.slow *= c.tomSlow;
   t.pMain = wrap(t.pMain + f * c.invSr);
   t.pLow = wrap(t.pLow + f / kTomRatio1[which] * c.invSr);
   t.pTop = wrap(t.pTop + f * (kTomRatio3[which] / kTomRatio1[which]) * c.invSr);

   const float k = 2.65f, norm = 1.0f / std::tanh(2.65f);
   const float kt = 4.0f, normT = 1.0f / std::tanh(4.0f);
   const float mainEnv = std::tanh(c.tomKnee[which] * t.env) * c.tomKneeNorm[which];
   // The comparator's square: high while the triangle rises.
   const float sq = (t.pMain < 0.25 || t.pMain >= 0.75) ? 1.0f : -1.0f;
   const float y = mainEnv * diodes(tri(t.pMain), k, norm) - kTomSquare * t.square * sq +
                   kTomLowLevel[which] * t.lowEnv * tri(t.pLow) +
                   kTomTopLevel[which] * t.top * diodes(tri(t.pTop), kt, normT);
   t.env *= c.tomEnv[which];
   t.lowSrc *= c.tomLowEnv[which];
   t.lowEnv += (t.lowSrc - t.lowEnv) * c.tomLowLag[which];
   t.top *= c.tomTop[which];
   t.square *= c.tomSquare;

   // The noise: a short wash under the hit and C54's tick when the trigger
   // ends.
   if (t.trig > 0 && --t.trig == 0)
      t.tick = 1.0f;
   const float nz = svfHp(t.hp, noise) * (kTomNoiseWash * t.noise + kTomNoiseTick * t.tick);
   t.noise *= c.tomNoise;
   t.tick *= c.tomTick;
   if (t.env < 1.0e-5f && t.lowSrc < 1.0e-5f && t.lowEnv < 1.0e-5f)
      t.active = false;
   return (y * t.amp + 2.0f * mP.tomNoise * t.noiseAmp * nz) * mP.tomLevel[which] * kTomGain;
}

void DrumEngine::Resonator::set(float hz, float q, float gain, float sr) {
   // The input plus gain times a band-pass whose peak is one, by the bilinear
   // transform warped to the centre.
   const float w = static_cast<float>(kTwoPi) * std::min(hz, 0.45f * sr) / sr;
   const float alpha = std::sin(w) / (2.0f * q);
   const float a0 = 1.0f + alpha;
   b0 = (1.0f + alpha + gain * alpha) / a0;
   b1 = -2.0f * std::cos(w) / a0;
   b2 = (1.0f - alpha - gain * alpha) / a0;
   a1 = b1;
   a2 = (1.0f - alpha) / a0;
}

float DrumEngine::tickRs() {
   Rs &r = mRs;
   if (!r.active)
      return 0.0f;
   const DrumCoefs &c = mC;
   const float e = r.excite;
   r.excite *= c.rsExcite;
   // The three resonators, summed through R408, R415 and R417 (12 k, 12 k,
   // 3.3 k) onto the node D91 and D92 clamp, which R423 and R418 also load.
   constexpr float g1 = 1.0f / 12.0f, g3 = 1.0f / 3.3f, gl = 1.0f / 12.68f;
   constexpr float norm = 1.0f / (2.0f * g1 + g3 + gl);
   const float sum = (g1 * r.f1.tick(e) + g1 * r.f2.tick(e) + g3 * r.f3.tick(e)) * norm;
   const float clamped = kRsClamp * std::tanh(sum * (1.0f / kRsClamp));
   // Q65, inverting on the way to IC50a, and not symmetrically.
   const float out = -(clamped > 0.0f ? kRsAsym * clamped : clamped) * r.gate;
   r.gate *= c.rsGate;
   ++r.time;
   if (r.gate < 1.0e-5f && r.time > static_cast<long>(0.05f * c.sr))
      r.active = false;
   return out * mP.rsLevel * kRsGain;
}

float DrumEngine::tickCp(float noise) {
   Cp &p = mCp;
   if (!p.active)
      return 0.0f;
   const DrumCoefs &c = mC;
   const float bp = svfBp(p.bp, noise) * (1.0f / 2.13f) * 1.6f;
   // The sawtooth envelope: four bursts, each cut off by the next. Each falls
   // steeply for a millisecond and then more slowly, as recorded.
   if (p.next < 4 && p.time >= c.cpOnset[p.next]) {
      p.burst = 1.0f;
      p.burstFast = 1.0f;
      if (++p.next == 4) {
         // The tail comes in with the last burst.
         p.tailD = 1.0f;
         p.tailA = 1.0f;
      }
   }
   // C56 and R202 come after the VCA, so they take out what the envelope's
   // edges would otherwise add underneath.
   const float wide = svfBp(p.bpWide, noise) * (1.0f / kCpBurstQ) * 1.6f;
   const float claps = p.hp2.tick(p.hp.tick(wide * (p.burst + kCpBurstFast * p.burstFast)));
   p.burst *= c.cpBurst;
   p.burstFast *= c.cpBurstFast;
   const float tailEnv = p.tailD - p.tailA;
   p.tailD *= c.cpTailDecay;
   p.tailA *= c.cpTailRise;
   const float tail = p.lp.tick(bp) * tailEnv * 2.6f;
   ++p.time;
   if (p.next >= 4 && p.burst < 1.0e-5f && p.tailD < 1.0e-5f)
      p.active = false;
   return (claps + kCpTail * tail) * p.amp * mP.cpLevel * kCpGain;
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
