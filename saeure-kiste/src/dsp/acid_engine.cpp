#include "acid_engine.h"

#include <cmath>
#include <cstring>

#include "plugincore/dsp/fastmath.h"

namespace saeurekiste {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// ---------------------------------------------------------------- the ladder
//
// The four ladder stages on the main board do not share a capacitor value.
// Reading them off the schematic, bottom to top: C18 = .018 uF on the input
// pair Q12, then C19, C24 and C26 = .033 uF each. The stages are driven by one
// current, so each pole sits at 1 / (2 pi R C) and the odd one out is the first
// one the signal meets:
//
//    0.033 / 0.018 = 1.8333
//
// So a 303's ladder is three coincident poles plus a fourth nearly an octave
// above them. That is not a detail: it is the whole reason the filter measures
// closer to 18 dB per octave than to 24 near its corner and only reaches the
// full four-pole slope an octave up. Every "the 303 is an 18 dB filter"
// argument is about this capacitor.
constexpr float kInputPoleRatio = 0.033f / 0.018f;

// Where the loop phase reaches 180 degrees with poles at w, w, w and 1.8333 w,
// the feedback needed is 4.25 rather than the 4.0 of four equal poles, and the
// ring lands at 1.16 w. The maximum here stops just below that, because the
// machine stops below it: the ringing waveform the service notes print for the
// TM3 alignment -- taken at resonance full clockwise -- dies away. A 303 that
// sustains a tone is a modified 303.
constexpr float kMaxFeedback = 4.15f;

// ------------------------------------------------------------------ envelopes
//
// "DECAY VR MAX T = 2.5 sec, MIN T = 200 ms", printed beside the envelope curve
// on the schematic. Read the curve as well as the caption: its vertical axis is
// marked 100 % and 10 %, so T is the time to a tenth, which is -20 dB and not
// the -60 dB that decayCoef() takes. Three times the knob is therefore the
// right argument, and getting this wrong makes every envelope in the
// instrument three times too fast.
constexpr float kDecayToSixtyDb = 3.0f;
// An accented note does not use the knob at all: the ACCENT line gates IC12, a
// 4066 analog switch, onto the envelope node through R117 and three diodes,
// which puts the decay at the short end whatever VR6 is set to. Which value it
// lands on is the Acc Decay control, defaulting to 200 ms -- the knob's own
// minimum. Setting it equal to Decay is the modification the Devil Fish added a
// switch for.

// C41 .1 uF through R134 22k = 2.2 ms, which is the VCA envelope's rise.
constexpr float kVcaAttackSec = 0.0022f;
// The gate falling. Short, and audible as the clipped end of a staccato note.
constexpr float kVcaReleaseSec = 0.006f;
// R123 1.5 M with C42 1 uF = 1.5 s, an RC time constant. Over a sixteenth note
// this is almost nothing, which is the point: the 303's VCA holds while the
// filter falls.
constexpr float kVcaSagSec = 1.5f;

// ---------------------------------------------------------------- the accent
//
// C62 is 1 uF and discharges through R138, 68 k. 68 ms is therefore how long an
// accent takes to leave the filter, and it is the reason a line breathes: at a
// hundred and thirty beats a minute a sixteenth is 115 ms, so the tail of one
// accent is still there when the next note starts.
//
// How much of the capacitor each accent fills could not be read off the scan --
// it depends on the accent pulse's width against the charging path, and neither
// is printed -- so it is the Acc Build control, defaulting to 0.75. How far an
// accent pushes the cutoff and how much it adds to the level are not printed
// either: Acc Sweep and Acc Gain.
//
// Acc Sweep is worth a word. The octaves have to carry the accent on their own,
// and they have to carry it past the shortened decay: an accented note runs a
// 200 ms envelope against an unaccented note's 600 ms one, so a small cutoff
// boost leaves the accent measuring *darker* than the note it is supposed to be
// accenting. It did, at two octaves. Three and a half is where an accented note
// comes out brighter over its whole length rather than only at its onset.

// -------------------------------------------------------------- env mod (Q9)
//
// Page 8 of the service notes, "VCF ENVELOPE MODULATION". Raising VR5 feeds
// more envelope to the base of Q10 *and* moves the bias Q9 sets, which lowers
// the resting cutoff -- Roland's own text calls it a gimmick and says why: a
// modulation that only ever opens the filter spends most of its travel in a
// range where nothing interesting happens, so the circuit drops the starting
// point as it deepens the sweep.
//
// The shape is the schematic's. The two magnitudes are not in it, so they are
// the Env Bias and Env Depth controls; their defaults, 1.5 and 5.0 octaves,
// are chosen so that the sweep covers the audible range at every setting of the
// knob, which is what the note says the circuit achieves. Env Bias at zero
// turns the gimmick off and leaves an ordinary envelope-amount knob.

// ------------------------------------------------------------------- the VCO
//
// The saw swings 12 V to 5.5 V and the square 8 V to 5 V, both printed beside
// their waveforms on the schematic: 6.5 V against 3.0 V, so the square reaches
// the filter 6.7 dB down. Reproduced rather than normalised away, because that
// difference is part of what the waveform switch does.
constexpr float kSquareLevel = 3.0f / 6.5f;

// The printed square is not flat: its top slopes down across the half period,
// at 110 Hz, by roughly a third. A first-order highpass at 25 Hz produces that,
// and that is the Droop control's default. Which RC in the circuit does it
// could not be traced from the scan -- it is the one number here fitted to a
// *drawing* rather than read off a component, which is exactly why it is a
// control and not a constant.

constexpr float kDcBlockHz = 12.0f;

// ------------------------------------------------------------- the Devil Fish
//
// The ladder's input pair is linear over the swing the machine gives it and
// starts switching above that. 1.5 is comfortably above the oscillator's own
// peak -- the falling ramp reaches 1.0 and the band-limited step overshoots it
// a little -- so at the stock Overdrive setting this stage is the identity and
// the engine is bit-for-bit what it was.
constexpr float kLadderInputKnee = 1.5f;

// The two positions of the Muffler switch, as the level above which each starts
// to bite. The amplifier's output peaks around 0.8 and spends its loudest few
// percent above 0.47, so Soft catches accents and the occasional peak while
// Hard works over most of the top of the signal -- which is what "affects
// sounds which are louder than usual" has to mean in numbers.
//
// The clipper takes the whole signal and not a split band, which took a wrong
// turn to establish. Routing the bottom of the spectrum around it -- the
// literal reading of "allowing the bass to pass largely unaffected" -- makes
// the Muffler *duller*, because then it only ever shaves the harmonics: the
// measured spectral centroid fell by two thirds and nothing buzzed. A clipper
// works on everything, and the bass survives because the knee is soft and the
// fundamental is what drives the clipping in the first place; the flattened
// peaks are where Whittle's "square wave clipping buzz" comes from.
constexpr float kMufflerSoftKnee = 0.45f;
constexpr float kMufflerHardKnee = 0.22f;

// How far full Filter FM moves the cutoff, at full amplifier output. Whittle
// gives no number for this -- there is no schematic for the Devil Fish -- so it
// is chosen to reach the behaviour he describes: edge at a little, and chaos at
// a lot.
constexpr float kFilterFmOctaves = 4.0f;

// The accent sweep's three speeds, as multiples of the Sweep Time control and
// of how far the charge may rise. Normal is the machine and is therefore 1 and
// 1. Fast and Slow are fitted to Whittle's description of what they do, not to
// components, because he documents the behaviour rather than the circuit.
constexpr float kSweepFastScale = 0.33f;
constexpr float kSweepSlowScale = 3.0f;
constexpr float kSweepSlowCeiling = 2.0f;

// How often the filter coefficients are recomputed. Every eighth sample is
// 6 kHz at 48 k, far above anything the envelopes do, and it keeps one tan()
// out of the per-sample path.
constexpr uint32_t kControlBlock = 8;

// Drift. Two slow sines at rates with no common factor, so the wander never
// repeats inside a take, plus one offset drawn per note. The scalings are in
// cents at Drift = 100 %: a machine that is a quarter tone out with itself by
// the end of a long note is a machine that needs a service, so the continuous
// part is smaller than the per-note part.
constexpr float kDriftSlowCents = 18.0f;
constexpr float kDriftPerNoteCents = 14.0f;
constexpr double kDriftRateA = 0.13;
constexpr double kDriftRateB = 0.071;

inline float exp2fast(float x) { return std::exp2(x); }

// The per-sample factor of a capacitor discharging through a resistor, for a
// stated RC time constant: one time constant leaves 1/e of the charge.
//
// This is NOT decayCoef(), which takes the time to reach -60 dB. The two differ
// by a factor of 6.9, and using decayCoef() for an RC made the accent sweep run
// out in ten milliseconds instead of sixty-eight -- which measured as an
// accented note coming out *darker* than an unaccented one, because all that
// was left of the accent by then was its shortened decay. Anything below that
// comes from a capacitor and a resistor on the schematic uses this.
inline float rcCoef(float seconds, float sampleRate) {
   if (seconds <= 0.0f)
      return 0.0f;
   return std::exp(-1.0f / (seconds * sampleRate));
}

} // namespace

// A tanh in all but name: monotonic, odd, and exactly +/-1 at +/-3, where it is
// clamped. Used for the ladder's saturation and for the drive stage.
float AcidEngine::softClip(float x) {
   x = clampv(x, -3.0f, 3.0f);
   const float x2 = x * x;
   return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Linear below the knee and compressing above it, asymptotically towards twice
// the knee. Continuous in value and in slope at the knee, so there is no edge
// where it starts working.
//
// The point of the knee is that a stage which is not being driven has to be
// left exactly alone. The ladder's input pair and the Muffler are both like
// that: the machine's own signal levels have to come through untouched, or
// turning a Devil Fish control to its stock position would not give back the
// stock sound.
float AcidEngine::softKnee(float x, float knee) {
   const float a = std::fabs(x);
   if (a <= knee)
      return x;
   const float over = a - knee;
   const float y = knee + over / (1.0f + over / knee);
   return x < 0.0f ? -y : y;
}

// The polynomial band-limited step, for the two discontinuities a ramp and a
// square have. Without it the oscillator folds its harmonics back down the
// spectrum, which is the one way a digital 303 gives itself away instantly.
float AcidEngine::polyBlep(float t, float dt) {
   if (dt <= 0.0f)
      return 0.0f;
   if (t < dt) {
      t /= dt;
      return t + t - t * t - 1.0f;
   }
   if (t > 1.0f - dt) {
      t = (t - 1.0f) / dt;
      return t * t + t + t + 1.0f;
   }
   return 0.0f;
}

void AcidEngine::prepare(double sampleRate, uint32_t /*maxBlockFrames*/) {
   mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
   reset();
   updateDerived();
}

void AcidEngine::reset() {
   mHeldCount = 0;
   mGate = false;
   mAccented = false;
   mPhase = 0.0f;
   std::memset(mZ, 0, sizeof(mZ));
   mVcfEnv = 0.0f;
   mVcaEnv = 0.0f;
   mVcaPeak = 0.0f;
   mAccentLevel = 0.0f;
   mAccentSweep = 0.0f;
   mCurrentHz = mTargetHz;
   mShotFrames = 0;
   mShotHolding = false;
   mSquareDroop.reset();
   mDcBlock.reset();
   mTone.reset();
   mLastOut = 0.0f;
   mDcBlock.setCutoff(kDcBlockHz, static_cast<float>(mSampleRate));
   // Seeded here rather than from a clock, so that reset() really does put the
   // engine back where it started and a render stays repeatable to the sample.
   mDriftRng.seed(0x303Au);
   mDriftTime = 0.0;
   mNoteDriftCents = 0.0f;
   mDriftCents = 0.0f;
   mVibAmount = 0.0f;
   mVibPhase = 0.0f;
   mVibAge = 0.0f;
   mVibCents = 0.0f;
   mModWheel = 0.0f;
}

void AcidEngine::setParams(const EngineParams &p) {
   mParams = p;
   updateDerived();
}

void AcidEngine::updateDerived() {
   const float sr = static_cast<float>(mSampleRate);

   mPitchRatio = std::exp2(mParams.tuningCents * (1.0f / 1200.0f));

   mDecayCoefOpen = decayCoef(mParams.decaySec * kDecayToSixtyDb, sr);
   mDecayCoefAccent = decayCoef(mParams.accDecaySec * kDecayToSixtyDb, sr);

   // An unaccented note opens at whatever Soft Attack says; an accented one
   // always uses the circuit's own 2.2 ms, because the modification's pot is
   // documented as acting on unaccented notes only.
   mVcaAttackCoef = onePoleCoef(mParams.softAttackSec, sr);
   mVcaAttackAccentCoef = onePoleCoef(kVcaAttackSec, sr);
   mVcaReleaseCoef = onePoleCoef(kVcaReleaseSec, sr);
   // Amp Decay is stated as the time to a tenth, like the filter's Decay knob,
   // and R123 x C42 is an RC: the two differ by ln(10).
   mVcaSagCoef = rcCoef(mParams.ampDecaySec / 2.302585093f, sr);

   // Sweep Speed changes the time constant as well as the charge law. Normal is
   // the machine and leaves C62 exactly where the schematic puts it.
   {
      const float scale = mParams.sweepSpeed == 1 ? kSweepFastScale
                        : mParams.sweepSpeed == 2 ? kSweepSlowScale
                                                  : 1.0f;
      mAccentSweepCoef = rcCoef(mParams.accentSweepSec * scale, sr);
   }

   mSlideCoef = onePoleCoef(mParams.slideSec, sr);

   // Q9's bias shift and the sweep depth move together, from one knob. How much
   // of each is Env Bias and Env Depth.
   mBaseCutoffHz = mParams.cutoffHz * exp2fast(-mParams.envMod * mParams.envBiasOct);
   mSweepOct = mParams.envMod * mParams.envDepthOct;

   // Res Range above 100 % takes the loop past the point where it oscillates,
   // which is a modification rather than a machine.
   mFeedback = mParams.resonance * kMaxFeedback * mParams.resRange;

   // Clamped away from zero rather than branched on: softClip(x * d) / d tends
   // to x as d falls, so a small drive is already the linear filter.
   mLadderDrive = mParams.ladder * 1.4f < 0.02f ? 0.02f : mParams.ladder * 1.4f;
   mLadderInv = 1.0f / mLadderDrive;

   mSquareDroop.setCutoff(clampv(mParams.droopHz, 0.5f, sr * 0.4f), sr);

   mTone.setCutoff(clampv(mParams.toneHz, 200.0f, sr * 0.45f), sr);

   mMufflerKnee = mParams.muffler == 1 ? kMufflerSoftKnee
                : mParams.muffler == 2 ? kMufflerHardKnee
                                       : 0.0f;
   // Level-matched at 0.8, the same nominal the drive stage uses, so switching
   // the Muffler in changes the shape of the sound and not how loud it is.
   // Without this Hard cost 5 dB and read as a volume control.
   mMufflerMakeup =
      mMufflerKnee > 0.0f ? 0.8f / softKnee(0.8f, mMufflerKnee) : 1.0f;

   // Zero switches the per-sample coefficient path off entirely, which is what
   // keeps a render with no Filter FM identical to one from before it existed.
   mFmOctaves = mParams.filterFm * kFilterFmOctaves;

   // 1 to 24 times into the clipper, level-matched so that turning it up
   // thickens instead of simply getting louder.
   mDrivePre = 1.0f + mParams.drive * 23.0f;
   // Level-matched at 0.8, and never above unity: the clipper already bounds
   // its output to +/-1, and a makeup above 1 would let the stage hand the
   // master something it cannot hold.
   mDriveMakeup = 0.8f / softClip(0.8f * mDrivePre);
   if (mDriveMakeup > 1.0f)
      mDriveMakeup = 1.0f;
}

// ------------------------------------------------------------------- notes

int AcidEngine::findHeld(int16_t key, int32_t noteId) const {
   for (int i = 0; i < mHeldCount; ++i) {
      const bool keyMatch = key < 0 || mHeld[i].key == key;
      const bool idMatch = noteId < 0 || mHeld[i].noteId < 0 || mHeld[i].noteId == noteId;
      if (keyMatch && idMatch)
         return i;
   }
   return -1;
}

void AcidEngine::startNote(int16_t key, bool accent, bool vibrato, bool slide) {
   mCurrentKey = key;
   mTargetHz = 440.0f * std::exp2((static_cast<float>(key) - 69.0f) * (1.0f / 12.0f));
   // A fresh offset per note: the converter is only ever approximately right,
   // and it is not wrong by the same amount twice.
   mNoteDriftCents = mDriftRng.white() * kDriftPerNoteCents;

   mVibAmount = vibrato ? 1.0f : 0.0f;
   // The vibrato delay restarts with the note, but not with a slide: a slid
   // note is a continuation, and restarting the delay would make the vibrato
   // drop out in the middle of a held phrase.
   if (!slide) {
      mVibAge = 0.0f;
      mVibPhase = 0.0f;
   }

   // An accent fires whether or not the step slides: on the machine it is the
   // accent bit of the step, and it reaches its own circuit rather than the
   // envelope generator.
   if (accent) {
      // Normal is the machine: the pulse tops C62 up towards full, and whatever
      // is left over from the last accent means the next one ends higher. That
      // is the behaviour people describe as the machine getting worked up.
      //
      // Fast is the other way round. The output is the pulse that was just
      // added rather than what has accumulated, so a residue makes the next one
      // *smaller*: the first accent of a run is the strongest.
      //
      // Slow charges more gently towards twice as far, and (through its time
      // constant) is still settling through the notes that follow.
      switch (mParams.sweepSpeed) {
      case 1:
         mAccentSweep = mParams.accBuild * (1.0f - mAccentSweep);
         break;
      case 2:
         mAccentSweep += (kSweepSlowCeiling - mAccentSweep) * mParams.accBuild * 0.5f;
         break;
      default:
         mAccentSweep += (1.0f - mAccentSweep) * mParams.accBuild;
         break;
      }
      mAccentLevel = mParams.accent;
   } else {
      mAccentLevel = 0.0f;
   }
   mAccented = accent;

   if (slide) {
      // The gate never went low, so nothing is retriggered: the filter
      // envelope carries straight on through the slide and the VCA does not
      // start again. This is the whole reason a slid 303 note sounds joined
      // rather than merely legato.
      mGate = true;
      ++mNoteCounter;
      return;
   }

   mCurrentHz = mTargetHz;
   mVcfEnv = 1.0f;
   mVcaPeak = 1.0f;
   mGate = true;
   ++mNoteCounter;
}

void AcidEngine::releaseGate() {
   mGate = false;
   mAccented = false;
}

void AcidEngine::noteOn(int16_t /*port*/, int16_t /*channel*/, int16_t key, int32_t noteId,
                        double velocity) {
   if (key < 0 || key > 127)
      return;
   const bool accent = mParams.accentHold ||
                       velocity * 127.0 >= static_cast<double>(mParams.accentVelocity);
   pushHeld(key, noteId);
   // Played from a host there is no per-step vibrato bit, so the mod wheel is
   // it: CC1 is where a vibrato lives on every other instrument.
   startNote(key, accent, mModWheel > 0.01f, mHeldBefore);
}

void AcidEngine::noteOnStep(int16_t key, bool accent, bool vibrato) {
   if (key < 0 || key > 127)
      return;
   pushHeld(key, -1);
   startNote(key, accent || mParams.accentHold, vibrato, mHeldBefore);
}

void AcidEngine::pushHeld(int16_t key, int32_t noteId) {
   mHeldBefore = mHeldCount > 0;
   if (mHeldCount < kStackSize) {
      mHeld[mHeldCount].key = key;
      mHeld[mHeldCount].noteId = noteId;
      ++mHeldCount;
   } else {
      // The stack is full, which takes sixteen simultaneously held notes on a
      // monophonic instrument. Drop the oldest rather than the newest.
      for (int i = 1; i < kStackSize; ++i)
         mHeld[i - 1] = mHeld[i];
      mHeld[kStackSize - 1].key = key;
      mHeld[kStackSize - 1].noteId = noteId;
   }
}

void AcidEngine::noteOff(int16_t /*port*/, int16_t /*channel*/, int16_t key, int32_t noteId) {
   const int idx = findHeld(key, noteId);
   if (idx < 0)
      return;
   for (int i = idx + 1; i < mHeldCount; ++i)
      mHeld[i - 1] = mHeld[i];
   --mHeldCount;

   if (mHeldCount == 0) {
      releaseGate();
      return;
   }
   // Fall back to whatever is still held, and slide to it: the gate stayed
   // high, so this is the same case as playing into a held note.
   const int16_t back = mHeld[mHeldCount - 1].key;
   mCurrentKey = back;
   mTargetHz = 440.0f * std::exp2((static_cast<float>(back) - 69.0f) * (1.0f / 12.0f));
}

void AcidEngine::choke(int16_t port, int16_t channel, int16_t key, int32_t noteId) {
   noteOff(port, channel, key, noteId);
   if (mHeldCount == 0) {
      // A choke is not a release: the sound is meant to stop, not to end.
      mVcaEnv = 0.0f;
      mVcaPeak = 0.0f;
   }
}

void AcidEngine::releaseAll() {
   mHeldCount = 0;
   releaseGate();
}

void AcidEngine::allSoundOff() {
   mHeldCount = 0;
   releaseGate();
   mVcaEnv = 0.0f;
   mVcaPeak = 0.0f;
   mVcfEnv = 0.0f;
   mAccentSweep = 0.0f;
   mShotFrames = 0;
   mShotHolding = false;
}

void AcidEngine::triggerShot() {
   // A low accented note, so the click on the version label plays the thing the
   // instrument is for rather than a bare tone.
   mShotHolding = true;
   mShotFrames = static_cast<uint32_t>(mSampleRate * 0.14);
   noteOnStep(36, true, false);
}

// ----------------------------------------------------------------- process

void AcidEngine::process(float *outL, float *outR, uint32_t frames) {
   const float sr = static_cast<float>(mSampleRate);
   const float invSr = 1.0f / sr;
   const float nyquistGuard = sr * 0.45f;

   uint32_t i = 0;
   while (i < frames) {
      if (mShotHolding) {
         if (mShotFrames == 0) {
            mShotHolding = false;
            noteOff(0, 0, 36, -1);
         } else {
            const uint32_t step = frames - i < mShotFrames ? frames - i : mShotFrames;
            mShotFrames -= step;
         }
      }

      const uint32_t n = (frames - i) < kControlBlock ? (frames - i) : kControlBlock;

      // ------------------------------------------------- control rate: drift
      if (mParams.drift > 0.0f) {
         const float wander =
            0.6f * static_cast<float>(std::sin(6.283185307 * kDriftRateA * mDriftTime)) +
            0.4f * static_cast<float>(std::sin(6.283185307 * kDriftRateB * mDriftTime + 1.3));
         mDriftCents = mParams.drift * (kDriftSlowCents * wander + mNoteDriftCents);
      } else {
         mDriftCents = 0.0f;
      }
      mDriftTime += static_cast<double>(n) / mSampleRate;

      // ------------------------------------------------ control rate: cutoff
      //
      // Everything that moves the ladder is summed in octaves, which is what
      // the antilog pair Q10/Q11 does with the voltages.
      const float trackOct =
         mParams.tracking * (static_cast<float>(mCurrentKey) - 60.0f) * (1.0f / 12.0f);
      const float octaves = mSweepOct * mVcfEnv +
                            mParams.accSweepOct * mParams.accent * mAccentSweep + trackOct;
      const float fcBase = clampv(mBaseCutoffHz * exp2fast(octaves), 20.0f, nyquistGuard);
      const float k = mFeedback;

      // Zero-delay one-pole coefficients. The first stage is the .018 uF one.
      //
      // With Filter FM off these are the whole block's, as they have always
      // been: the envelopes move far too slowly for eight samples to matter and
      // it keeps two tangents out of the inner loop. With Filter FM on the
      // cutoff moves at audio rate by definition, so they have to be redone
      // every sample -- that is what the control costs, and why it is off by
      // default.
      const bool fmOn = mFmOctaves > 0.0f;
      float G1 = 0.0f, Gr = 0.0f, Gprod = 0.0f;
      auto coefficientsFor = [&](float fc) {
         const float w1 = clampv(fc * kInputPoleRatio, 20.0f, nyquistGuard);
         const float g1 = std::tan(kPi * w1 / sr);
         const float gr = std::tan(kPi * fc / sr);
         G1 = g1 / (1.0f + g1);
         Gr = gr / (1.0f + gr);
         Gprod = G1 * Gr * Gr * Gr;
      };
      if (!fmOn)
         coefficientsFor(fcBase);

      for (uint32_t s = 0; s < n; ++s) {
         if (fmOn) {
            // The amplifier's output from the previous sample, which is what
            // makes this a feedback path and not an impossibility.
            const float fc =
               clampv(fcBase * exp2fast(mFmOctaves * mLastOut), 20.0f, nyquistGuard);
            coefficientsFor(fc);
         }
         // ------------------------------------------------------- envelopes
         mVcfEnv *= mAccented ? mDecayCoefAccent : mDecayCoefOpen;
         mAccentSweep *= mAccentSweepCoef;

         if (mGate) {
            // The sag falls towards Amp Sustain rather than towards nothing. At
            // zero -- the machine -- this is the bare multiply it always was.
            mVcaPeak = mParams.ampSustain +
                       (mVcaPeak - mParams.ampSustain) * mVcaSagCoef;
            const float target = mVcaPeak * (1.0f + mAccentLevel * mParams.accGain);
            mVcaEnv += (target - mVcaEnv) *
                       (mAccented ? mVcaAttackAccentCoef : mVcaAttackCoef);
         } else {
            mVcaEnv += (0.0f - mVcaEnv) * mVcaReleaseCoef;
         }

         // ------------------------------------------------------------- VCO
         mCurrentHz += (mTargetHz - mCurrentHz) * mSlideCoef;
         // Vibrato, per sample so the modulation is smooth, with the delay
         // ramping it in over 80 ms once it has waited its time.
         if (mVibAmount > 0.0f && mParams.vibratoCents > 0.0f) {
            mVibAge += invSr;
            mVibPhase += mParams.vibratoHz * invSr;
            if (mVibPhase >= 1.0f)
               mVibPhase -= 1.0f;
            const float since = mVibAge - mParams.vibratoDelaySec;
            const float ramp = since <= 0.0f ? 0.0f : (since >= 0.08f ? 1.0f : since * 12.5f);
            mVibCents = mParams.vibratoCents * mVibAmount * ramp * sin2piFast(mVibPhase);
         } else {
            mVibCents = 0.0f;
         }
         const float hz =
            clampv(mCurrentHz * mPitchRatio *
                      exp2fast((mDriftCents + mVibCents) * (1.0f / 1200.0f)),
                   8.0f, nyquistGuard);
         const float dt = hz / sr;

         mPhase += dt;
         if (mPhase >= 1.0f)
            mPhase -= 1.0f;

         float osc;
         if (mParams.waveform == 0) {
            // A falling ramp: the integrator runs down and Q8 snaps it back.
            osc = 1.0f - 2.0f * mPhase + polyBlep(mPhase, dt);
         } else {
            float t2 = mPhase + 0.5f;
            if (t2 >= 1.0f)
               t2 -= 1.0f;
            osc = mPhase < 0.5f ? 1.0f : -1.0f;
            osc += polyBlep(mPhase, dt);
            osc -= polyBlep(t2, dt);
            osc = mSquareDroop.tick(osc) * kSquareLevel;
         }

         // Overdrive: the oscillator's level into the ladder. The input pair is
         // linear over the swing the machine gives it and compresses above
         // that, so at the stock setting this is the identity.
         osc = softKnee(osc * mParams.oscDrive, kLadderInputKnee);

         // ------------------------------------------------------------- VCF
         //
         // Four one-pole stages with feedback around them, solved for the
         // current sample rather than delayed: the stage offsets are known, so
         // the loop is a linear equation in the last stage's output.
         const float S1 = (1.0f - G1) * mZ[0];
         const float S2 = (1.0f - Gr) * mZ[1];
         const float S3 = (1.0f - Gr) * mZ[2];
         const float S4 = (1.0f - Gr) * mZ[3];
         const float B = Gr * Gr * Gr * S1 + Gr * Gr * S2 + Gr * S3 + S4;

         const float y4 = (Gprod * osc + B) / (1.0f + Gprod * k);
         // The ladder is transistors, and transistors run out of headroom. This
         // is where a 303 gets its growl at high resonance rather than simply
         // whistling.
         const float fb = softClip(y4 * mLadderDrive) * mLadderInv;
         const float u1 = osc - k * fb;

         float v = G1 * (u1 - mZ[0]);
         const float y1 = v + mZ[0];
         mZ[0] = y1 + v;

         v = Gr * (y1 - mZ[1]);
         const float y2 = v + mZ[1];
         mZ[1] = y2 + v;

         v = Gr * (y2 - mZ[2]);
         const float y3 = v + mZ[2];
         mZ[2] = y3 + v;

         v = Gr * (y3 - mZ[3]);
         const float y = v + mZ[3];
         mZ[3] = y + v;

         // ------------------------------------------------------- VCA + out
         float out = y * mVcaEnv;

         out = softClip(out * mDrivePre) * mDriveMakeup;
         out = mTone.tick(out);

         // The Muffler goes last, after the drive stage, and that is a
         // deliberate departure from where Whittle draws it.
         //
         // On a Devil Fish the Muffler is on the amplifier's output because
         // that *is* the output -- there is nothing after it. This plugin has a
         // drive stage the machine does not have, and putting the Muffler in
         // front of it erased the Muffler completely: the drive's own clipper
         // is far harder, so it simply re-flattened whatever shape the Muffler
         // had made. Measured, the spectral centroid moved by 0.0 % at every
         // setting. Last in the chain, the Muffler is the final clipper, which
         // is the position it occupies on the hardware.
         //
         // It is linear below its knee, so Off and a quiet signal are both
         // untouched and a stock preset is bit-for-bit what it was.
         if (mMufflerKnee > 0.0f)
            out = softKnee(out, mMufflerKnee) * mMufflerMakeup;

         // What Filter FM reads: the amplifier's output after the Muffler, as
         // Whittle specifies.
         mLastOut = clampv(out, -4.0f, 4.0f);

         out = mDcBlock.tick(out) * mParams.gain;

         outL[i + s] = out;
         outR[i + s] = out;
      }

      i += n;
   }
}

double AcidEngine::tailSeconds() const {
   // The VCA release plus whatever the filter is still ringing with.
   return 0.25;
}

bool AcidEngine::isSilent() const {
   return !mGate && mVcaEnv < 1.0e-5f && !mShotHolding;
}

} // namespace saeurekiste
