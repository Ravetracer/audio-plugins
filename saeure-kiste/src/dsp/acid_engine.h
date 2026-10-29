#pragma once

// The synthesis engine: one monophonic voice modelled on the TB-303 main
// board as the February 1982 service notes draw it.
//
// Signal path, in the order the schematic has it:
//
//    pitch CV -> slide lag -> VCO (falling ramp | square)
//                              |   + the additions: sine, triangle, pulse,
//                              |     wavetable, noise, and up to 16 voices
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
// The two things here that sound random -- Drift and the Noise waveform --
// draw from generators seeded in reset(), so they repeat exactly as well.
//
// Unison is the one place the signal path is not the schematic's. With more
// than one voice and any Spread at all, the voices are panned across the two
// channels and each channel gets a ladder, an amplifier and a drive stage of
// its own (see Path below); everything that is not audio -- the envelopes, the
// accent, the slide -- is still one circuit driving both. With one voice, or
// no spread, there is one path and the engine is the machine it always was.

#include <atomic>
#include <cstdint>

#include "plugincore/dsp/filters.h"
#include "plugincore/dsp/rng.h"

#include "dsp/chain.h"
#include "dsp/drive.h"
#include "dsp/wavetable.h"

namespace saeurekiste {

// How often the filter coefficients are recomputed. Every eighth sample is
// 6 kHz at 48 k, far above anything the envelopes do, and it keeps one tan()
// out of the per-sample path. The chain's Pre and Post buses run a block at a
// time too, which is why it is declared here: the engine keeps one block of
// the oscillator for them.
constexpr uint32_t kControlBlock = 8;

// clampv, decayCoef, onePoleCoef and the one-pole filters are the suite's.
using namespace plugincore;

struct EngineParams {
   int waveform = 0;            // a WaveformKind: 0 sawtooth, 1 square, then the additions
   float tuningCents = 0.0f;    // VR2, +/- 700
   // The additions to the oscillator. None of them is on the machine, and
   // their defaults leave it alone: none of the five shape controls touches the
   // sawtooth or the square, and one unison voice is the oscillator as it was.
   // One field per waveform, each glided on its own, so changing waveform does
   // not sweep one control from where another was.
   float pulseWidth = 0.5f;     // Pulse: share of the cycle, 0.03..0.5
   float wtPosition = 0.0f;     // Wavetable: 0..1 through the frames
   float triSkew = 0.0f;        // Triangle: 0..1, apex towards the start
   float sineBend = 0.0f;       // Sine: 0..1, phase distortion
   float noiseGrain = 0.0f;     // Noise: 0..1, white to one value per cycle
   int wavetable = 0;           // a BuiltinTable, or kTableUser
   int unisonVoices = 1;        // 1..kMaxUnison
   float unisonDetuneCents = 15.0f; // the outermost voices' offset
   float unisonSpread = 0.5f;   // 0..1, how far across the stereo field
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

   // The effect chain. Every slot is Off until somebody sets one, and an
   // all-Off chain does no work.
   ChainParams chain;
};

class AcidEngine {
public:
   // Monophonic, like the machine. The host is told so through voice-info, and
   // overlapping notes are what produce a slide rather than a second voice.
   static constexpr int kMaxVoices = 1;
   // Unison voices are copies of the one oscillator, not notes.
   static constexpr int kMaxUnison = 16;

   void prepare(double sampleRate, uint32_t maxBlockFrames);
   void reset();
   // The continuous controls glide to what this hands over rather than jumping
   // to it; switches and times take effect at once. The first call after
   // prepare() or reset() is not a change and is taken as it is.
   void setParams(const EngineParams &p);

   // The table the Wavetable waveform reads when Table is User. Audio thread;
   // the plugin owns it and keeps it alive while it is set here. Null falls
   // back to the first built-in table.
   void setUserTable(const Wavetable *table);

   // One cycle of what the oscillator is making, for the window: `n` samples
   // of the shape at the level the filter receives it, without band-limiting
   // and without the square's droop. The same shape code as process() uses,
   // so the picture cannot drift from the sound.
   static void drawCycle(const EngineParams &p, const Wavetable *table, float *out, int n);

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

   // Called once at the top of every host block, before the first process().
   // The block is then processed in consecutive pieces, split at its events,
   // and the engine counts where in the block each piece starts.
   void beginBlock();

   void process(float *outL, float *outR, uint32_t frames);

   // The chain's Master bus, after the plugin's delay: the last thing in the
   // box. Called once over the whole block, after every process() of it; a
   // note that started in the block reaches its slots at the sample it
   // started on, because process() noted where that was.
   void processMaster(float *outL, float *outR, uint32_t frames);

   // The host's tempo, for the chain's synced times. Every block, because a
   // tempo is a property of the block rather than of the parameter table.
   void setTempo(double bpm) { mChain.setTempo(bpm); }

   // The amplifier's release plus the longest tail the chain's slots can hold.
   // Read by the host from any thread, so the chain's part is published.
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
   // Its integral: the same correction for a corner rather than a step.
   static float polyBlamp(float t, float dt);
   static float softClip(float x);
   // Linear up to `knee` and compressing above it, so a stage that is not being
   // driven is left exactly alone. Both the ladder's input pair and the Muffler
   // want that: they have to be bit-transparent at the settings the machine
   // itself uses and only bite past them.
   static float softKnee(float x, float knee);

   void updateDerived();
   // One voice's next sample before the square's droop, its phase moved on by
   // `dt`. `aux` is what the waveform needs besides: the band-limit level for
   // the wavetable, the hold increment for the noise, and nothing otherwise.
   float voiceSample(int v, float dt, float aux);
   // One control block's step of the glide from mParams towards mTarget, for
   // a block of `frames` samples.
   void glideParams(uint32_t frames);

   // ------------------------------------------------------------------- state
   double mSampleRate = 48000.0;

   // What the engine is running at, and what the controls last asked for. The
   // two differ only while a glide is under way.
   EngineParams mParams{};
   EngineParams mTarget{};
   bool mPrimed = false;          // whether setParams() has been called since reset()
   uint32_t mGlideLeft = 0;       // samples left before mParams is snapped to mTarget
   uint32_t mGlideTotal = 0;      // how many a glide takes, from the sample rate
   uint32_t mDriveGlideFrames = 0; // samples since the drive stage's settings last stepped
   float mGlideCoef = 1.0f;       // a full control block's step

   // What the drive stage was last derived for. Its setParams() runs a level
   // match through the curve that costs up to 30 us on the heaviest model, so it
   // is redone only when one of these has moved.
   int mDriveModelSet = -1;
   float mDriveSet = -1.0f;
   float mDriveBiasSet = -2.0f;

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

   // VCO. One phase per unison voice; voice 0 is the oscillator itself.
   float mPhase[kMaxUnison] = {0.0f};
   // Noise: a generator per voice, and the sample-and-hold Noise Grain puts on it.
   uint32_t mNoiseState[kMaxUnison] = {0};
   float mNoiseHold[kMaxUnison] = {0.0f};
   float mNoisePhase[kMaxUnison] = {0.0f};
   // Per voice, from the unison controls: its pitch ratio, its weights into the
   // two channels and log2 of its ratio, for the wavetable's level.
   float mVoiceRatio[kMaxUnison] = {0.0f};
   float mVoiceOct[kMaxUnison] = {0.0f};
   float mVoiceL[kMaxUnison] = {0.0f};
   float mVoiceR[kMaxUnison] = {0.0f};
   int mVoices = 1;
   float mUnisonNorm = 1.0f;      // 1 / sqrt(voices)
   float mPulseWidth = 0.5f;      // from Pulse Width
   float mTriApex = 0.5f;         // from Tri Skew
   float mSineKnee = 0.5f;        // from Sine Bend
   float mSineOffset = 0.0f;      // the bent sine's DC, taken out
   float mSineScale = 1.0f;       // and its peak put back at 1
   const Wavetable *mUserTable = nullptr;
   const Wavetable *mTable = nullptr; // what the Wavetable waveform reads

   // Everything from the oscillator's output to the engine's: the ladder, the
   // amplifier's output stages, the drive, the tone and the DC blocker. One
   // per channel. Only the first runs unless unison is spreading voices, and
   // the second starts as a copy of it, so switching it in does not click.
   struct Path {
      OnePoleHp squareDroop;      // the tilt the printed square wave shows
      // VCF -- four one-pole stages, zero delay feedback.
      float z[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      // The drive stage: eight models out of the literature, in dsp/drive.h.
      // Everything about it -- the shapes, their gain staging, their filters
      // and the oversampling around them -- lives there rather than here,
      // because none of it comes from the service notes the rest of this file
      // is built from.
      DriveStage drive;
      OnePoleLp tone;
      OnePoleHp dcBlock;
      // Filter FM reads the amplifier's output, so it reads it one sample
      // late -- which is what makes it a feedback path rather than an
      // impossibility.
      float lastOut = 0.0f;
   };
   Path mPath[2];
   bool mStereo = false;          // whether the second path is running

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

   // The Muffler's knee -- the level above which it starts to bite, 0 when it is
   // off -- and the gain that puts back what the compression took, so the
   // switch changes the shape of the sound rather than its level.
   float mMufflerKnee = 0.0f;
   float mMufflerMakeup = 1.0f;

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

   // The chain. It sits inside the sample path at two of its three buses, so
   // the oscillator and the filter are worked out in two passes over a block
   // and these are what connects them; see process(). Both buffers hold one
   // control block, and the second channel is a copy when unison is not
   // spreading anything.
   Chain mChain;
   float mOscBuf[2][kControlBlock] = {};
   float mVcaEnvBuf[kControlBlock] = {};
   // A note starts at the beginning of a process() call, because the host's
   // events are split there; these say that one did, once for the Pre and
   // Post buses and once for the Master's.
   bool mHitPending = false;
   bool mMasterHitPending = false;

   // Where in the host's block the Master bus's notes started. process() is
   // called over consecutive pieces of the block, so the offset is counted.
   static constexpr int kMaxMasterHits = 64;
   void noteMasterHit();
   uint32_t mBlockFrame = 0;
   int mMasterHits[kMaxMasterHits] = {};
   int mMasterHitCount = 0;

   // How long the chain may still be sounding; see trackChainTail().
   static constexpr float kChainTailFloor = 1.0e-5f;
   static constexpr double kChainTailHoldSeconds = 0.25;
   void trackChainTail(const float *l, const float *r, uint32_t n);
   uint32_t mChainRingFrames = 0;
   std::atomic<double> mChainTailSeconds{0.0};
};

} // namespace saeurekiste
