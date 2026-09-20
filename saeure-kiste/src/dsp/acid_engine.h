#pragma once

// The synthesis engine: one monophonic voice modelled on the TB-303 main
// board as the February 1982 service notes draw it.
//
// Signal path, in the order the schematic has it:
//
//    pitch CV -> slide lag -> VCO (falling ramp | square)
//                              |
//                              v
//                     4-stage transistor ladder  <- cutoff CV
//                       (C18 .018, C19/C24/C26 .033)   |
//                              |                       |
//                              v            envelope, accent sweep, Q9 bias
//                             VCA  <- gate envelope + accent level
//                              |
//                              v
//                        drive -> tone -> volume
//
// Everything that carries a number from the schematic says so at the point it
// is used. tools/analysis/README.md collects all of them in one place.
//
// The engine holds no random state at all, so two renders of the same preset
// at the same sample rate are bit-identical. That is unusual for this code
// base, where the nature instruments this window and parameter model came from
// are deliberately stochastic -- and it is what a machine emulation should be.

#include <cstdint>

#include "plugincore/dsp/filters.h"
#include "plugincore/dsp/rng.h"

#include "dsp/drive.h"

namespace saeurekiste {

// clampv, decayCoef, onePoleCoef and the one-pole filters are the suite's.
using namespace plugincore;

struct EngineParams {
   int waveform = 0;            // 0 sawtooth, 1 square
   float tuningCents = 0.0f;    // VR2, +/- 700
   float cutoffHz = 500.0f;     // VR3, the nominal ladder pole
   float resonance = 0.35f;     // VR4, 0..1
   float envMod = 0.5f;         // VR5, 0..1
   float decaySec = 0.6f;       // VR6, 0.2..2.5 s
   float tracking = 0.0f;       // not on the machine; 0 is the machine
   float accent = 0.6f;         // VR7, 0..1
   float accentVelocity = 100.0f; // MIDI velocity at which a note is accented
   float accentSweepSec = 0.068f; // C62 x R138
   float slideSec = 0.06f;
   float drive = 0.2f;          // not on the machine
   float toneHz = 8000.0f;      // not on the machine
   int distType = 0;            // DriveModel: 0 is the soft clipper it had
   float distBias = 0.0f;       // -1..+1, the model's operating point
   float distMix = 1.0f;        // 0 bypasses the stage, 1 is all of it
   float gain = 0.5f;           // VR8, linear

   // Mods. Every one of these was a constant in acid_engine.cpp, and every one
   // of them is a number the schematic does not give. The defaults are the
   // values the engine shipped with.
   float envBiasOct = 1.5f;     // how far Q9's bias shift drops the resting cutoff
   float envDepthOct = 5.0f;    // how deep a full Env Mod sweep is
   float accSweepOct = 3.5f;    // how far a full accent opens the filter
   float accBuild = 0.75f;      // how much of C62 one accent fills
   float accGain = 0.9f;        // how much louder an accent is
   float accDecaySec = 0.2f;    // the decay an accented note is forced to
   float droopHz = 25.0f;       // the tilt on the square
   float ladder = 1.0f;         // how hard the ladder feedback saturates
   float resRange = 1.0f;       // >1 lets the filter oscillate
   float drift = 0.0f;          // oscillator instability

   // Vibrato. Not on the machine at all -- it is one of the things people added
   // to theirs, per step, and it is driven per step here too.
   float vibratoCents = 25.0f;
   float vibratoHz = 6.0f;
   float vibratoDelaySec = 0.06f;

   // The Devil Fish. Robin Whittle's modification of the machine, taken from
   // his own manual; see params.cpp for where each number comes from. Every
   // default here is the stock circuit, so an engine that is handed none of
   // them behaves exactly as it did before they existed.
   float oscDrive = 1.0f;          // oscillator level into the ladder, 1 = stock
   float filterFm = 0.0f;          // the VCA's output back into the cutoff
   int muffler = 0;                // 0 off, 1 soft, 2 hard
   float softAttackSec = 0.0022f;  // the VCA's attack on an unaccented note
   float ampDecaySec = 3.4527f;    // the VCA's sag, to a tenth
   float ampSustain = 0.0f;        // what the sag falls towards
   int sweepSpeed = 0;             // 0 normal, 1 fast, 2 slow
   bool accentHold = false;        // accent every note, whatever it asked for
};

class AcidEngine {
public:
   // Monophonic, like the machine. The host is told so through voice-info, and
   // overlapping notes are what produce a slide rather than a second voice.
   static constexpr int kMaxVoices = 1;

   void prepare(double sampleRate, uint32_t maxBlockFrames);
   void reset();
   void setParams(const EngineParams &p);

   void noteOn(int16_t port, int16_t channel, int16_t key, int32_t noteId, double velocity);
   // The sequencer's way in. Accent and vibrato are the step's own bits rather
   // than something inferred from a velocity, because on the machine they were
   // bits and not a velocity. `slide` is not a parameter here for the same
   // reason it is not one in the MIDI path: a slide is what happens when the
   // previous note has not been released yet, and the sequencer produces one by
   // overlapping the notes, exactly as a slid step does.
   void noteOnStep(int16_t key, bool accent, bool vibrato);
   void noteOff(int16_t port, int16_t channel, int16_t key, int32_t noteId);
   void choke(int16_t port, int16_t channel, int16_t key, int32_t noteId);
   void allSoundOff();
   // Drops every held note and closes the gate, without cutting the sound: the
   // amplifier releases as it would at the end of a phrase. The sequencer uses
   // it to guarantee its notes are gone, because a stale entry in the held
   // stack makes every later note look like a slide.
   void releaseAll();
   // How many notes the engine thinks are held. The sequencer checks it against
   // its own bookkeeping.
   int heldCount() const { return mHeldCount; }
   // CC1. In MIDI mode it is where the vibrato comes from, since there is no
   // step to carry a vibrato bit.
   void setModWheel(float v) { mModWheel = clampv(v, 0.0f, 1.0f); }

   // One free note, for the window's version label. Released by the engine
   // itself a moment later, so it needs no matching note-off.
   void triggerShot();

   void process(float *outL, float *outR, uint32_t frames);

   double tailSeconds() const;
   bool isSilent() const;

   // Published to the window: the activity meter and the header ornament.
   uint32_t activeVoiceCount() const { return mGate ? 1u : 0u; }
   uint32_t noteCounter() const { return mNoteCounter; }

private:
   // ---------------------------------------------------------------- held notes
   //
   // Last-note priority with a held stack, which is what a monophonic
   // instrument driven from a piano roll needs: releasing the upper of two
   // held notes falls back to the lower one, and does so by sliding, because
   // on the machine the gate never went low between them.
   static constexpr int kStackSize = 16;

   struct HeldNote {
      int16_t key;
      int32_t noteId;
   };

   int findHeld(int16_t key, int32_t noteId) const;
   void pushHeld(int16_t key, int32_t noteId);
   void startNote(int16_t key, bool accent, bool vibrato, bool slide);
   void releaseGate();

   // ------------------------------------------------------------------ helpers
   static float polyBlep(float t, float dt);
   static float softClip(float x);
   // Linear up to `knee` and compressing above it, so a stage that is not being
   // driven is left exactly alone. Both the ladder's input pair and the Muffler
   // want that: they have to be bit-transparent at the settings the machine
   // itself uses and only bite past them.
   static float softKnee(float x, float knee);

   void updateDerived();

   // ------------------------------------------------------------------- state
   double mSampleRate = 48000.0;

   EngineParams mParams{};

   HeldNote mHeld[kStackSize]{};
   int mHeldCount = 0;
   // Whether anything was held when the current note arrived, which is the
   // whole test for a slide.
   bool mHeldBefore = false;
   float mModWheel = 0.0f;

   bool mGate = false;
   bool mAccented = false;        // the note currently sounding
   float mTargetHz = 110.0f;      // where the slide is heading
   float mCurrentHz = 110.0f;     // where it is
   float mSlideCoef = 1.0f;
   int16_t mCurrentKey = 45;

   // VCO
   float mPhase = 0.0f;
   OnePoleHp mSquareDroop;        // the tilt the printed square wave shows
   OnePoleHp mDcBlock;

   // VCF -- four one-pole stages, zero delay feedback.
   float mZ[4] = {0.0f, 0.0f, 0.0f, 0.0f};

   // Envelopes
   float mVcfEnv = 0.0f;          // decay only: triggered, then it falls
   float mVcfEnvCoef = 0.0f;
   float mVcaEnv = 0.0f;
   float mVcaPeak = 0.0f;         // the slow sag while the gate is held
   float mVcaAttackCoef = 0.0f;      // an unaccented note: the Soft Attack control
   float mVcaAttackAccentCoef = 0.0f; // an accented one: always the circuit's 2.2 ms
   float mVcaReleaseCoef = 0.0f;
   float mVcaSagCoef = 0.0f;
   float mAccentLevel = 0.0f;     // the sounding note's accent, for the VCA
   float mAccentSweep = 0.0f;     // C62's charge, for the filter
   float mAccentSweepCoef = 0.0f;

   // Post
   OnePoleLp mTone;
   // The drive stage: eight models out of the literature, in dsp/drive.h.
   // Everything about it -- the shapes, their gain staging, their filters and
   // the oversampling around them -- lives there rather than here, because
   // none of it comes from the service notes the rest of this file is built
   // from.
   DriveStage mDriveStage;

   // The Muffler's knee -- the level above which it starts to bite, 0 when it is
   // off -- and the gain that puts back what the compression took, so the
   // switch changes the shape of the sound rather than its level.
   float mMufflerKnee = 0.0f;
   float mMufflerMakeup = 1.0f;

   // Filter FM reads the amplifier's output, so it reads it one sample late --
   // which is what makes it a feedback path rather than an impossibility.
   float mLastOut = 0.0f;
   float mFmOctaves = 0.0f;       // 0 when Filter FM is off, and then the
                                  // coefficients stay on the control block

   // Vibrato. Per note, because the step carries it.
   float mVibAmount = 0.0f;   // 0..1: the step's bit, or the mod wheel
   float mVibPhase = 0.0f;
   float mVibAge = 0.0f;      // seconds since the note started, for the delay
   float mVibCents = 0.0f;

   // Drift. Two slow sines a machine's worth apart, plus one offset per note.
   // Seeded at reset, so a render is still repeatable to the sample.
   RngLite mDriftRng;
   double mDriftTime = 0.0;
   float mNoteDriftCents = 0.0f;
   float mDriftCents = 0.0f;

   // Derived once per parameter change
   float mDecayCoefOpen = 0.0f;   // from VR6
   float mDecayCoefAccent = 0.0f; // the fixed short decay an accent forces
   float mBaseCutoffHz = 500.0f;  // VR3 after the Q9 bias shift
   float mSweepOct = 0.0f;        // how far the envelope moves it
   float mFeedback = 0.0f;        // the ladder's k
   float mLadderDrive = 1.4f;     // how hard the feedback is pushed into softClip
   float mLadderInv = 1.0f / 1.4f;
   float mPitchRatio = 1.0f;      // VR2

   uint32_t mNoteCounter = 0;
   uint32_t mShotFrames = 0;      // the free note's remaining gate, in samples
   bool mShotHolding = false;
};

} // namespace saeurekiste
