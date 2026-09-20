#include "dsp/drive.h"

#include <cmath>

namespace saeurekiste {

namespace {

// [Pirkle] table 19.1, ATAN: y = atan(kx) / atan(k). Normalised, so the shape
// changes with k while the output level does not.
inline float atanShaper(float x, float k) {
   const float kk = k < 0.1f ? 0.1f : k;
   return std::atan(kk * x) / std::atan(kk);
}

// A first-order low shelf as a lowpass sum: y = x + (g - 1) * lp(x). What
// [Pirkle] 19.12 puts after each triode to stand in for the cathode bypass
// capacitor, at 88 Hz and -12 dB.
constexpr float kValveShelfHz = 88.0f;
constexpr float kValveShelfGain = 0.2512f; // 10^(-12/20)

// The DC-removing highpass [Pirkle] 19.12 puts after the waveshaper, standing
// in for the plate's coupling capacitor. Asymmetric shaping produces an
// offset, and in a cascade the next stage would amplify it.
constexpr float kValveHpHz = 12.0f;

// The class-A operating point: the grid sits off centre, so the two halves of
// the wave are not amplified equally. See valveStage().
constexpr float kValveGridBias = 0.12f;

// [DAFX] M-file 4.4 calls these rh and rl: "placement of poles in the HP
// filter which removes the DC component" and "the pole placement in the LP
// filter used to simulate capacitances in a tube amplifier". The M-file leaves
// both to the caller; these are the corners they correspond to here. The LP is
// deliberately gentle -- Tone sits right after this stage and is the control
// that belongs to the player.
constexpr float kTubeHpHz = 12.0f;
constexpr float kTubeLpHz = 6500.0f;

// The EQ [Pirkle] 19.13 puts between the third and the fourth triode. The
// corners are the ones its own plugin ships with (table 19.12), 500 Hz and
// 4 kHz; the shelves are 9 dB rather than the 6 of its screenshot, which is
// inside the +/-20 dB the book gives the control and is what makes this model
// a voice rather than another clipper. This is the tone stack of a guitar
// preamp and it is most of why a cascade of triodes sounds like an amplifier:
// it thins the bottom before the last stage and lifts the top after it.
constexpr float kValveEqLowHz = 500.0f;
constexpr float kValveEqLowGain = 0.355f; // 10^(-9/20)
constexpr float kValveEqHighHz = 4000.0f;
constexpr float kValveEqHighGain = 2.818f; // 10^(+9/20)

// Rectification leaves a large offset behind ([DAFX] 4.3.3, figure 4.36), so
// the rectifier model needs its own highpass rather than leaving it to the
// amplifier's DC blocker at the end of the chain.
constexpr float kRectHpHz = 30.0f;

inline float sgn(float x) { return x < 0.0f ? -1.0f : 1.0f; }

} // namespace

// --------------------------------------------------------------- the shapes

// The stage this plugin has always had: a rational tanh, clamped at +/-3.
// Kept exactly as it was, and kept first in the list, because it is what every
// preset written before the models existed sounds like. It is also the one
// model that does not run at twice the sample rate, for the same reason: a
// preset from 0.5.0 has to render the same samples it always did.
float DriveStage::softClipLegacy(float x) {
   x = clampv(x, -3.0f, 3.0f);
   const float x2 = x * x;
   return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// [DAFX] equation 4.14, after Schetzen: symmetrical soft clipping in three
// zones. Below a third of full scale it is *linear* with a gain of two, from a
// third to two thirds it compresses, and above that it is flat.
//
// The linear zone is the whole point of this one and the reason it does not
// sound like the others: a quiet note passes through it untouched and a loud
// one is squashed, so the stage follows the playing instead of flattening
// everything equally.
float DriveStage::overdriveDafx(float x) {
   const float a = std::fabs(x);
   if (a < 1.0f / 3.0f)
      return 2.0f * x;
   if (a <= 2.0f / 3.0f) {
      const float t = 2.0f - 3.0f * a;
      return sgn(x) * (3.0f - t * t) / 3.0f;
   }
   return sgn(x);
}

// [DAFX] equation 4.13, after Bendiksen: asymmetric soft clipping for valve
// simulation, with a work point Q and a distortion figure.
//
//   f(x) = (x - Q) / (1 - e^(-dist*(x - Q))) + Q / (1 - e^(dist*Q))   x != Q
//   f(Q) = 1/dist + Q / (1 - e^(dist*Q))
//
// The model limits large negative values and stays roughly linear for positive
// ones, which is exactly the lopsided transfer curve a triode has, and it is
// where the even harmonics come from. At the book's own Q = -0.2 and dist = 8
// the positive half reaches 0.95 while the negative half is held at 0.25.
//
// It is not bounded for large positive inputs -- the M-file normalises its
// input first -- so the input is clamped to +/-1 here, which is that
// normalisation done in advance.
float DriveStage::tubeDafx(float x) const {
   const float q = clampv(x, -1.0f, 1.0f);
   const float d = q - mTubeQ;
   float y;
   if (std::fabs(d) < 1.0e-6f) {
      y = 1.0f / mTubeDist;
   } else {
      const float denom = 1.0f - std::exp(-mTubeDist * d);
      y = std::fabs(denom) < 1.0e-9f ? d : d / denom;
   }
   return y + mTubeQOffset;
}

// [DAFX] equation 4.15: f(x) = sgn(x) * (1 - e^-|x|), with the asymmetry of
// [Pirkle] table 19.1's FEXP1 and equation 19.2 -- the negative half is given
// its own gain and its own limit, which is how both sources describe a Fuzz
// Face: the positive peak clips first and the negative one clips lower.
//
// Exponential from the first volt, so unlike a clipper it never has a linear
// region at all. That is what "fuzz" means here and why it sounds gated.
float DriveStage::fuzzDafx(float x, float gain, float asymmetry) {
   const float g = clampv(1.0f - 0.65f * asymmetry, 0.2f, 1.8f);
   if (x >= 0.0f) {
      const float k = gain;
      return (1.0f - std::exp(-k * x)) / (1.0f - std::exp(-k));
   }
   const float k = gain / g;
   return -g * (1.0f - std::exp(k * x)) / (1.0f - std::exp(-k));
}

// [DAFX] section 4.3.3 and [Pirkle] table 19.2 (HWR, FWR): rectification.
//
// Half-wave rectification keeps the positive half and generates even
// harmonics; full-wave takes the absolute value, which doubles the number of
// zero crossings and therefore the fundamental -- an octave up, with the
// original fundamental gone from the spectrum ([DAFX] figure 4.36).
//
// This is the one model here that changes the *pitch* of what it is given
// rather than its edge, which is why it is in the set: it is the furthest
// thing from a clipper that the sources describe, and nobody can mistake it
// for one. Bias picks the kind of rectification, Drive how much of it is
// mixed with the straight signal.
float DriveStage::rectifier(float x) const {
   const float clipped = softClipLegacy(x);
   const float full = std::fabs(clipped);
   const float half = 0.5f * (clipped + std::fabs(clipped));
   // Bias runs the rectifier from half-wave at one end to full-wave at the
   // other; at the centre it is half of each.
   const float blend = clampv(0.5f + 0.5f * mBias, 0.0f, 1.0f);
   const float rect = half + (full - half) * blend;
   return clipped + (2.0f * rect - clipped) * mRectAmount;
}

// [Pirkle] equation 19.1: QL = 2 / (2^N - 1), y = QL * int(x / QL).
//
// Kept because it is in the book, because it is the only model here that is
// digital rather than a circuit, and because there is no mistaking it for
// anything else. It runs inside the oversampler like the rest, which takes the
// worst of the aliasing off the steps.
float DriveStage::crush(float x) const {
   const float q = x + mCrushOffset;
   return mCrushStep * static_cast<float>(static_cast<int>(q / mCrushStep));
}

// One class-A triode stage of [Pirkle] 19.12: waveshaper, inversion, the
// DC-removing highpass and the cathode-bypass low shelf. The filters are the
// stage's own and are what make three of these in series sound like an
// amplifier rather than like one clipper driven three times as hard.
float DriveStage::valveStage(float x, float bias) const {
   // The offset is the grid bias, and it is what makes the stage class A: the
   // valve sits off centre so the two halves of the wave are amplified
   // differently. [Pirkle] 19.2 ("the top half of the waveform is amplified
   // differently from the bottom half") and [DAFX] 4.3.1 on the triode's
   // quadratic curve both describe it; neither gives a figure for a 303, so
   // the centre of Bias is the operating point and Bias moves it.
   return atanShaper(x + kValveGridBias + bias, mValveSat);
}

// --------------------------------------------------------------- the stage

void DriveStage::prepare(double sampleRate) {
   mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
   mOversampler.prepare(mSampleRate);

   // Every filter inside a model runs at twice the base rate, because that is
   // where the models run.
   const float twice = static_cast<float>(mSampleRate * 2.0);
   mTubeHp.setCutoff(kTubeHpHz, twice);
   mTubeLp.setCutoff(clampv(kTubeLpHz, 200.0f, twice * 0.45f), twice);
   mRectHp.setCutoff(kRectHpHz, twice);
   for (int i = 0; i < kValveStages; ++i) {
      mValveHp[i].setCutoff(kValveHpHz, twice);
      mValveShelf[i].setCutoff(kValveShelfHz, twice);
   }
   mValveEqLow.setCutoff(kValveEqLowHz, twice);
   mValveEqHigh.setCutoff(kValveEqHighHz, twice);
   reset();
}

void DriveStage::reset() {
   mOversampler.reset();
   mTubeHp.reset();
   mTubeLp.reset();
   mRectHp.reset();
   for (int i = 0; i < kValveStages; ++i) {
      mValveHp[i].reset();
      mValveShelf[i].reset();
   }
   mValveEqLow.reset();
   mValveEqHigh.reset();
}

float DriveStage::shapeOnly(float x) const {
   switch (mModel) {
   case kDriveSoftClip:
   default:
      return softClipLegacy(x * mPre) * mMakeup;
   case kDriveOverdrive:
      return overdriveDafx(x * mPre + mBias * 0.3f) * mMakeup;
   case kDriveTube:
      return tubeDafx(x * mPre) * mMakeup;
   case kDriveValveStack: {
      float y = x * mPre;
      for (int i = 0; i < kValveStages; ++i)
         y = -valveStage(y, mBias * 0.2f);
      return y * mMakeup;
   }
   case kDriveFuzz:
      return fuzzDafx(x * mPre, mFuzzGain, mFuzzAsym) * mMakeup;
   case kDriveRectifier:
      return rectifier(x * mPre) * mMakeup;
   case kDriveCrush:
      return crush(x) * mMakeup;
   }
}

// The model as the signal path hears it: the shape plus whatever filtering the
// source puts around it, all of it at twice the sample rate.
float DriveStage::process(float x) {
   // Soft Clip is the legacy path and stays on it: no oversampling, no
   // filters, the same arithmetic it has always had. That is what keeps a
   // preset written before any of this render the samples it always did.
   if (mModel == kDriveSoftClip)
      return softClipLegacy(x * mPre) * mMakeup * mTrim;

   switch (mModel) {
   case kDriveOverdrive:
      return mOversampler.tick(x, [this](float s) {
                return overdriveDafx(s * mPre + mBias * 0.3f) * mMakeup;
             }) * mTrim;

   case kDriveTube:
      // [DAFX] M-file 4.4: the shape, then the highpass that removes the DC
      // the asymmetry leaves behind, then the lowpass that stands in for the
      // valve's capacitances.
      return mOversampler.tick(x, [this](float s) {
                const float shaped = tubeDafx(s * mPre) * mMakeup;
                return mTubeLp.tick(mTubeHp.tick(shaped));
             }) * mTrim;

   case kDriveValveStack:
      // [Pirkle] 19.13: three class-A triode stages in series, each one
      // inverting, each with its own DC highpass and cathode-bypass shelf.
      return mOversampler.tick(x, [this](float s) {
                float y = s * mPre;
                for (int i = 0; i < kValveStages; ++i) {
                   // The tone stack sits between the third stage and the
                   // fourth, which is where the book puts it.
                   if (i == kValveStages - 1) {
                      y = y + (kValveEqLowGain - 1.0f) * mValveEqLow.tick(y);
                      y = y + (kValveEqHighGain - 1.0f) * mValveEqHigh.tick(y);
                   }
                   y = -valveStage(y, mBias * 0.2f);
                   y = mValveHp[i].tick(y);
                   y = y + (kValveShelfGain - 1.0f) * mValveShelf[i].tick(y);
                }
                // The output stage. A preamp does not hand its last triode
                // straight to the world -- there is a power amp after it, and
                // both sources model that as a further saturation ([DAFX]
                // figure 4.24 on class A/AB output stages, [Pirkle] 19.2 on
                // the power amp that follows the cascade). Here it also does
                // the job the level matching cannot: the tone stack's 9 dB of
                // treble lift lands on harmonics the stages in front of it
                // just made, and without something bounding the result this
                // model peaked half again as high as any other.
                return softClipLegacy(y * mMakeup * mTrim);
             });

   case kDriveFuzz:
      return mOversampler.tick(x, [this](float s) {
                return fuzzDafx(s * mPre, mFuzzGain, mFuzzAsym) * mMakeup;
             }) * mTrim;

   case kDriveRectifier:
      return mOversampler.tick(x, [this](float s) {
                return mRectHp.tick(rectifier(s * mPre) * mMakeup);
             }) * mTrim;

   case kDriveCrush:
      return mOversampler.tick(x, [this](float s) { return crush(s) * mMakeup; }) * mTrim;

   default:
      return softClipLegacy(x * mPre) * mMakeup * mTrim;
   }
}

// Gain staging, one mapping per model.
//
// This is where the first attempt at this stage went wrong and it is worth
// saying why: every shape was given the same pre-gain, so every shape crossed
// its knee at the same setting and saturated at the same setting, and a
// saturated clipper is a square wave whatever curve produced it. The knob then
// moved all of them through the same three states together. Each model's own
// parameter -- Schetzen's thresholds, Bendiksen's `dist`, Pirkle's saturation,
// a bit depth -- is what Drive moves here, over the range its source gives it.
void DriveStage::setParams(int model, float drive, float bias, float mix) {
   mModel = model < 0 ? 0 : (model >= kNumDriveModels ? 0 : model);
   mDrive = clampv(drive, 0.0f, 1.0f);
   mBias = clampv(bias, -1.0f, 1.0f);
   mMix = clampv(mix, 0.0f, 1.0f);

   mTrim = 1.0f;
   mMakeup = 1.0f;

   switch (mModel) {
   case kDriveSoftClip:
   default:
      // 1 to 24 times into the clipper, exactly as before.
      mPre = 1.0f + mDrive * 23.0f;
      break;

   case kDriveOverdrive:
      // The curve's linear zone ends at a third of full scale and it is flat
      // above two thirds, so the whole of its character lives in a range of
      // about 2:1. Drive walks the signal across exactly that: past 1.8 there
      // is nothing left of the model but a square wave, which is what the
      // first version of this stage got wrong for every one of its shapes.
      mPre = 1.0f + mDrive * 0.9f;
      break;

   case kDriveTube:
      // [DAFX] figure 4.26 draws the curve at Q = -0.2 and dist = 8; Bias sits
      // at the book's work point when it is centred and moves it from there.
      // Q must stay negative or the model loses the asymmetry it exists for.
      mTubeQ = clampv(-0.2f + mBias * 0.18f, -0.45f, -0.02f);
      mTubeDist = 1.0f + mDrive * 15.0f;
      mTubeQOffset = mTubeQ / (1.0f - std::exp(mTubeDist * mTubeQ));
      mPre = 1.0f + mDrive * 1.2f;
      break;

   case kDriveValveStack:
      // [Pirkle] 19.14 gives the plugin's Drive control a range of 1 to 10 on
      // the triode's saturation. Three stages of it in series is already a lot
      // of gain -- each one shapes what the last one shaped -- so the range
      // here is the lower half of the book's and the input level barely moves.
      mValveSat = 1.0f + mDrive * 1.0f;
      mPre = 1.0f + mDrive * 0.25f;
      break;

   case kDriveFuzz:
      // FEXP1's k, over the range that takes it from a rounded edge to a gate.
      //
      // The asymmetry does not start at zero, and that is the model rather
      // than a preference: both sources describe a fuzz as asymmetric --
      // [DAFX] 4.3.2 on the Fuzz Face ("the negative clipping level is lower
      // than the positive clipping value") and [Pirkle] 19.3 on the Super
      // Overdrive's three diodes. Bias moves it from there, either way.
      mFuzzGain = 0.8f + mDrive * 4.0f;
      mFuzzAsym = clampv(0.7f + mBias * 0.5f, -1.0f, 1.0f);
      mPre = 1.0f;
      break;

   case kDriveRectifier:
      // How much of the rectified signal is mixed in. The straight signal is
      // still there at low settings, so the octave comes up underneath the
      // note rather than replacing it.
      mRectAmount = mDrive;
      mPre = 1.0f + mDrive * 2.0f;
      break;

   case kDriveCrush: {
      // [Pirkle] equation 19.1. Twelve bits down to three: above twelve the
      // quantisation is below the noise floor of everything else here and the
      // model is a very expensive piece of wire.
      const float bits = 9.0f - mDrive * 7.0f;
      mCrushStep = 2.0f / (std::exp2(bits) - 1.0f);
      mCrushOffset = mBias * mCrushStep * 0.5f;
      mPre = 1.0f;
      break;
   }
   }

   // ------------------------------------------------------------- loudness
   //
   // Two different jobs, and the first version of this stage did only one of
   // them. Matching *peaks* keeps the plugin inside its bounds; it does not
   // keep two models at the same loudness, because a model that squashes one
   // half of the wave (Tube) or thins the bottom of it (Valve Stack) has the
   // same peak and much less signal under it. Measured on a bass line, the
   // models sat as much as 14 dB apart, and at that distance a comparison
   // between them is a comparison of volume.
   //
   // So the matching is done on RMS, through the model's own curve, and then
   // limited by the peak -- loudness first, safety second.
   const float peakOf = [this]() {
      float peak = 0.0f;
      for (int i = 0; i <= 32; ++i) {
         const float amp = 0.8f * (static_cast<float>(i) / 32.0f);
         const float up = std::fabs(shapeOnly(amp));
         const float down = std::fabs(shapeOnly(-amp));
         const float y = up > down ? up : down;
         if (y > peak)
            peak = y;
      }
      return peak;
   }();

   if (mModel == kDriveSoftClip) {
      // The legacy stage, matched at 0.8 and never above unity, which is the
      // arithmetic it has always used -- to the bit, which is what keeps every
      // preset written before the models existed rendering what it always did.
      mMakeup = peakOf > 1.0e-6f ? 0.8f / peakOf : 1.0f;
      if (mMakeup > 1.0f)
         mMakeup = 1.0f;
      return;
   }

   // A cycle of a sine at the level this instrument's amplifier hands over,
   // through the curve, in and out.
   float inSum = 0.0f;
   float outSum = 0.0f;
   for (int i = 0; i < 64; ++i) {
      const float x = 0.55f * std::sin(6.283185307f * static_cast<float>(i) / 64.0f);
      const float y = shapeOnly(x);
      inSum += x * x;
      outSum += y * y;
   }
   const float rmsMatch = outSum > 1.0e-9f ? std::sqrt(inSum / outSum) : 1.0f;
   // And the ceiling that keeps the stage inside the amplifier: the loudest
   // the curve gets, brought to 0.9.
   const float peakLimit = peakOf > 1.0e-6f ? 0.9f / peakOf : 1.0f;
   mMakeup = rmsMatch < peakLimit ? rmsMatch : peakLimit;
   if (mMakeup > 8.0f)
      mMakeup = 8.0f;

   // Headroom for the models with edges in them. The amplifier's DC blocker is
   // a highpass and a highpass overshoots on an edge, which is what put the
   // plugin's output over its bound the first time the hard shapes were
   // measured. Applied here rather than inside the shape, because the level
   // matching above would otherwise measure it and take it straight back out.
   // ------------------------------------------------------- the last gain
   //
   // What the curve-based matching above cannot see: the models' own filters,
   // and the fact that this instrument's signal is a bass line rather than a
   // sine. The Valve Stack's tone stack takes 9 dB off everything below
   // 500 Hz and a 303 line lives down there; the rectifier moves its energy an
   // octave up and out of the fundamental; the quantiser throws away
   // everything below its first step.
   //
   // These are one number per model, measured through the whole plugin on the
   // Machine Running preset at 55 % drive, and chosen so that all six land
   // within about a decibel of each other -- because a comparison between two
   // distortions at different levels is a comparison of level, and the louder
   // one always wins it.
   //
   // Soft Clip is not in the list and is louder than all of them at the same
   // setting: its pre-gain reaches 24 and it is compressing hard by the middle
   // of the knob. That is the stage this instrument has always had and it is
   // not going to be quietened to make a table look tidy.
   switch (mModel) {
   case kDriveOverdrive:
      mTrim = 1.20f;
      break;
   case kDriveTube:
      mTrim = 1.34f;
      break;
   case kDriveValveStack:
      mTrim = 3.30f;
      break;
   case kDriveFuzz:
      mTrim = 1.12f;
      break;
   case kDriveRectifier:
      mTrim = 1.65f;
      break;
   case kDriveCrush:
      mTrim = 1.56f;
      break;
   default:
      mTrim = 1.0f;
      break;
   }
}

} // namespace saeurekiste
