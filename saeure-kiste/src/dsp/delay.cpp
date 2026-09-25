#include "dsp/delay.h"

#include <cmath>

namespace saeurekiste {

namespace {

// Below this a repeat is inaudible and the tail has ended. -100 dBFS: far
// below anything a listener can hear under the next note, and far above the
// denormal range, which the whole process() call runs with flushed anyway.
constexpr float kTailFloor = 1.0e-5f;

// How long the tail is held open after the last audible repeat. One second is
// longer than the gap between two repeats at any delay time the free range
// offers, so a slow echo is never cut off between two of its own taps.
constexpr double kTailHoldSeconds = 1.0;

} // namespace

void DelayStage::prepare(double sampleRate) {
   mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
   const size_t n = static_cast<size_t>(mSampleRate * kMaxDelaySeconds) + 4;
   mLeft.resize(n);
   mRight.resize(n);
   // 50 ms to reach the new delay time. Short enough that the control feels
   // connected, long enough that the glide is a slide rather than a click.
   mSlew = onePoleCoef(0.05f, static_cast<float>(mSampleRate));
   // 20 ms, the engine's glide, so every control on the instrument moves alike.
   mParamSlew = onePoleCoef(0.02f, static_cast<float>(mSampleRate));
   reset();
}

void DelayStage::reset() {
   mLeft.clear();
   mRight.clear();
   mWrite = 0;
   mRingFrames = 0;
   mPrimed = false;
}

void DelayStage::setParams(bool on, int mode, float timeSec, float feedback, float mix,
                           float width) {
   mOn = on;
   mMode = mode < 0 || mode >= kNumDelayModes ? kDelayStereo : mode;
   mFeedbackTarget = feedback < 0.0f ? 0.0f : feedback;
   mMixTarget = on ? clampv(mix, 0.0f, 1.0f) : 0.0f;
   mWidthTarget = clampv(width, 0.0f, 2.0f);

   // One sample is the shortest a line can be read at without the read head
   // passing the write head; the top is whatever the buffer holds.
   const float maxLen = static_cast<float>(mLeft.buffer.size()) - 2.0f;
   const float samples = clampv(timeSec * static_cast<float>(mSampleRate), 1.0f, maxLen);
   mLeft.target = samples;
   mRight.target = mMode == kDelayStereo ? clampv(samples * kStereoRatio, 1.0f, maxLen) : samples;
   // The first time round, the heads start where they are pointed rather than
   // gliding up to it from zero.
   // The same holds for the level controls.
   if (!mPrimed) {
      mPrimed = true;
      mLeft.length = mLeft.target;
      mRight.length = mRight.target;
      mFeedback = mFeedbackTarget;
      mMix = mMixTarget;
      mWidth = mWidthTarget;
   }
}

void DelayStage::process(float *left, float *right, uint32_t frames) {
   if (!left || !right || mLeft.buffer.empty())
      return;

   // Off, or fully dry, and done gliding there: nothing to add. The buffers
   // are left holding whatever was last written so that switching the stage
   // back on picks up where it was rather than from silence -- but the tail is
   // declared over, because nothing of it is being heard.
   if (mMix <= 0.0f && mMixTarget <= 0.0f) {
      mRingFrames = 0;
      return;
   }

   const size_t size = mLeft.buffer.size();
   const uint32_t hold = static_cast<uint32_t>(mSampleRate * kTailHoldSeconds);

   float loudest = 0.0f;

   for (uint32_t i = 0; i < frames; ++i) {
      const float dryL = left[i];
      const float dryR = right[i];

      mLeft.length += (mLeft.target - mLeft.length) * mSlew;
      mRight.length += (mRight.target - mRight.length) * mSlew;
      mFeedback += (mFeedbackTarget - mFeedback) * mParamSlew;
      mMix += (mMixTarget - mMix) * mParamSlew;
      mWidth += (mWidthTarget - mWidth) * mParamSlew;

      // [musicdsp] 256, the volume-adjusted form. Per sample, because Width
      // may be gliding.
      const float span = 1.0f + mWidth;
      const float norm = 1.0f / (span > 2.0f ? span : 2.0f);
      const float coefM = norm;
      const float coefS = mWidth * norm;

      const float tapL = mLeft.read(mWrite);
      const float tapR = mRight.read(mWrite);

      // [DAFX] eq 2.61, y(n) = c x(n) + g y(n - M), one line per channel --
      // except that g goes past the stability condition the equation carries,
      // so the loop is closed through the soft clipper. Below unity that is a
      // straight multiply and the structure is exactly the book's; past it the
      // repeats saturate instead of growing without bound.
      float inL, inR;
      switch (mMode) {
      case kDelayMono: {
         // One line, fed the sum. The right-hand buffer is written with the
         // same thing so that switching to a stereo mode does not start from
         // an empty half.
         const float mono = (dryL + dryR) * 0.5f;
         inL = inR = mono + softClip(tapL * mFeedback);
         break;
      }
      case kDelayPingPong: {
         // [Pirkle] figure 14.13 crosses both the input and the feedback to the
         // opposite channel. The crossed feedback is taken as printed; the
         // crossed *input* is not, and cannot be, because the book's figure
         // assumes a stereo source. This instrument is monophonic and puts the
         // same signal in both channels, so crossing the inputs would feed both
         // lines the same thing, both taps would stay equal for ever, and the
         // mode would produce no ping-pong at all -- just the mono delay with
         // extra arithmetic.
         //
         // What a mono source needs instead is the input on one side only:
         // the sum goes into the left line, the left line feeds the right, the
         // right feeds the left. The repeats then alternate L, R, L, R, which
         // is the figure-of-eight the figure describes and the reason the mode
         // is there.
         const float mono = (dryL + dryR) * 0.5f;
         inL = mono + softClip(tapR * mFeedback);
         inR = softClip(tapL * mFeedback);
         break;
      }
      case kDelayStereo:
      default:
         // [Pirkle] figure 14.12: two independent lines. What makes it stereo
         // is that the right one runs at kStereoRatio of the left.
         inL = dryL + softClip(tapL * mFeedback);
         inR = dryR + softClip(tapR * mFeedback);
         break;
      }

      mLeft.buffer[mWrite] = inL;
      mRight.buffer[mWrite] = inR;
      if (++mWrite >= size)
         mWrite = 0;

      // In mono both channels read the left-hand line, so the two sides are
      // identical, the side signal is zero and Width has nothing to open --
      // which is the right answer rather than a special case. Taking it from
      // the left line explicitly is what keeps that true through the first
      // block after a switch out of a stereo mode, while the right-hand buffer
      // still holds what that mode put there.
      const float wetL = tapL;
      const float wetR = mMode == kDelayMono ? tapL : tapR;
      const float mid = (wetL + wetR) * coefM;
      const float side = (wetR - wetL) * coefS;
      // Through the suite's output stage, and only the wet signal goes through
      // it. Two things upstream can push a repeat past full scale -- feedback
      // past unity adds the dry input on top of an already-clipped tap, and
      // the width matrix amplifies whatever the two sides disagree about -- so
      // something has to bound it. Clipping here rather than on the finished
      // mix is what keeps the dry instrument untouched: a crossfade between
      // two signals that are each inside the rails is inside them too.
      const float outL = softClip(mid - side);
      const float outR = softClip(mid + side);

      const float peak = std::fabs(outL) > std::fabs(outR) ? std::fabs(outL) : std::fabs(outR);
      if (peak > loudest)
         loudest = peak;

      // The same crossfade the drive stage's Dist Mix uses, so the two mix
      // controls on this instrument behave identically.
      left[i] = dryL + (outL - dryL) * mMix;
      right[i] = dryR + (outR - dryR) * mMix;
   }

   // A one-pole never quite arrives, and the stage only switches itself off
   // once the mix is exactly where it was sent.
   auto settle = [](float &cur, float target) {
      if (std::fabs(target - cur) < 1.0e-5f)
         cur = target;
   };
   settle(mFeedback, mFeedbackTarget);
   settle(mMix, mMixTarget);
   settle(mWidth, mWidthTarget);

   if (loudest > kTailFloor)
      mRingFrames = hold;
   else if (mRingFrames > frames)
      mRingFrames -= frames;
   else
      mRingFrames = 0;
}

} // namespace saeurekiste
