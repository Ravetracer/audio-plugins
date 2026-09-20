#pragma once

// The delay: the second part of this instrument that is not the TB-303.
//
// The machine has no delay and never had one. Like the drive stage in
// drive.h, this therefore comes from the literature rather than from the
// service notes, and every part of it is cited where it is used:
//
//   [DAFX]     U. Zoelzer (ed.), "DAFX: Digital Audio Effects", 2nd edition,
//              Wiley 2011, section 2.5.2 "IIR comb filter" (Dutilleux,
//              Holters, Disch, Zoelzer), equations 2.61 and 2.62, and section
//              2.5.4 "Fractional delay lines".
//   [Pirkle]   W. Pirkle, "Designing Audio Effect Plugins in C++", 2nd
//              edition, Routledge 2019, chapter 14 "Delay Effects and
//              Circular Buffers", sections 14.4.1 and 14.4.2 with figures
//              14.12 and 14.13.
//   [musicdsp] M. Gruhn, "Stereo Width Control (Obtained Via Transformation
//              Matrix)", musicdsp.org archive entry 256, 2008. Placed by its
//              author in the public domain. The volume-adjusted form from the
//              comments is the one used here.
//
// Neither book is in this repository and neither may be: they are copyrighted
// and are not ours to redistribute. What is here is the equations.
//
// The one place this deliberately leaves its sources is the feedback. [DAFX]
// states the stability condition for equation 2.61 outright -- |g| <= 1, or
// "the signal would grow endlessly" -- and the whole point of the Feedback
// control going to 130 % is to break it. So the loop is closed through the
// suite's own soft clipper instead: past unity the repeats stop growing and
// start saturating, which is a sound rather than an overflow. That is the only
// thing in here that is not out of a book, and it is the reason the stage
// cannot run away.

#include <cstdint>
#include <vector>

#include "plugincore/dsp/fastmath.h"

namespace saeurekiste {

using namespace plugincore;

// How the two lines are wired. [Pirkle] figures 14.12 and 14.13 are the second
// and third of these.
enum DelayMode {
   kDelayMono = 0,  // one line, the same repeats in both channels
   kDelayStereo,    // two lines at a ratio of each other, straight feedback
   kDelayPingPong,  // two lines, input and feedback crossed
   kNumDelayModes
};

// The longest delay the buffer can hold, in seconds. Two seconds is the free
// range's top; the extra is headroom for a synced half note, which is three
// seconds at 40 BPM and is clamped to this above that.
constexpr double kMaxDelaySeconds = 3.0;

// What a stereo delay's right-hand line runs at, as a fraction of the left.
//
// [Pirkle] 14.4.2 makes the point that two lines at the *same* time are not a
// stereo delay at all -- "if the delay times are identical across channels,
// then the ping-pong effect is usually lost" -- and that the ratios worth
// having are the simple ones. Two thirds is 3:2, which puts the right-hand
// repeats exactly between the left-hand ones and is what makes the mode
// rhythmic rather than merely wide. It is fixed rather than a control because
// this panel has no room for a tenth knob; see TODO.md.
constexpr float kStereoRatio = 2.0f / 3.0f;

class DelayStage {
public:
   void prepare(double sampleRate);
   void reset();

   // `timeSec` is the left-hand line's delay, already resolved from whichever
   // of the free and synced controls is in charge. `feedback` is 0..1.3 and
   // `mix` and `width` are 0..1 and 0..2, as the parameters give them.
   void setParams(bool on, int mode, float timeSec, float feedback, float mix, float width);

   // In place, over a whole block. The stage runs after everything else in the
   // plugin, so this is the last thing to touch the buffers.
   void process(float *left, float *right, uint32_t frames);

   // Whether repeats are still audible. The host is told the plugin may sleep
   // when the engine falls silent, and a delay that was still ringing when
   // that happened would be cut off mid-tail.
   bool ringing() const { return mRingFrames > 0; }

private:
   // One line's worth of buffer and its read head. The head is smoothed rather
   // than jumped to, so moving Time -- or the host moving the tempo -- glides
   // the repeats the way a tape delay does instead of clicking.
   struct Line {
      std::vector<float> buffer;
      float length = 0.0f;  // where the read head is, in samples behind write
      float target = 0.0f;  // where it is heading

      void resize(size_t n) { buffer.assign(n, 0.0f); }
      void clear() { std::fill(buffer.begin(), buffer.end(), 0.0f); }

      // [DAFX] 2.5.4: a fractional delay read as a linear interpolation
      // between the two samples either side of it.
      inline float read(uint32_t write) const {
         const float len = length;
         const size_t size = buffer.size();
         const float back = static_cast<float>(write) - len;
         const float wrapped = back < 0.0f ? back + static_cast<float>(size) : back;
         const size_t i0 = static_cast<size_t>(wrapped);
         const float frac = wrapped - static_cast<float>(i0);
         const size_t a = i0 >= size ? 0 : i0;
         const size_t b = a + 1 >= size ? 0 : a + 1;
         return buffer[a] + (buffer[b] - buffer[a]) * frac;
      }
   };

   Line mLeft, mRight;
   uint32_t mWrite = 0;
   double mSampleRate = 48000.0;

   bool mOn = false;
   int mMode = kDelayStereo;
   float mFeedback = 0.0f;
   float mMix = 0.0f;
   float mWidth = 1.0f;
   float mSlew = 1.0f;  // how fast the read heads follow their target
   // Whether a delay time has been set since the last reset. The read heads
   // glide to a new one, which is the point of them, but the *first* one is
   // not a change -- gliding up to it from zero would smear the first repeat
   // across the fifty milliseconds before it and put it in the wrong place.
   bool mPrimed = false;

   // Frames left before the tail counts as over. Refreshed whenever a repeat
   // is still above the threshold, so feedback at or past unity simply never
   // lets it reach zero -- which is correct: those repeats never die.
   uint32_t mRingFrames = 0;
};

} // namespace saeurekiste
