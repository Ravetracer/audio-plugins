#pragma once

// The effect chain: sixteen slot positions, each of which sits in front of the
// ladder, after the amplifier, or on the master bus, and runs in slot order
// within the bus it chose.
//
// Sixteen is a pool, not a demand: the window shows the slots that hold an
// effect and an ADD button, so a chain of two effects is two strips and the
// other fourteen cost nothing but their buffers. A slot whose type is Off is
// skipped entirely by process(), which is what keeps an unused chain free.
//
// Why it is a bus per slot rather than one serial chain at the end: the whole
// point of this instrument is what the oscillator does *into* the ladder, so a
// bandpass in front of it is a different sound from the same bandpass after
// the amplifier. Three buses and a choice per slot gives that without a graph
// editor -- VCO -> bandpass -> saturator -> ladder -> EQ -> VCA -> drive ->
// chain -> delay is expressible, and so is putting everything at the end,
// which is what an empty chain does.
//
// Ported from Substrike's effect slots, which is where these effects were
// written and measured. Nothing is linked across plugin folders here, so this
// is a copy and belongs to SaeureKiste from now on; see chain_dsp.h for the
// sources of the building blocks and drive.h for the models the Saturator
// slot runs.
//
// An empty chain -- every slot Off, which is every factory preset and every
// blob written before this existed -- does no work at all: the engine's
// process loop calls into it and it returns having touched nothing, so the
// instrument still renders bit for bit what it did.

#include <array>
#include <cstddef>
#include <vector>

#include "dsp/chain_dsp.h"
#include "dsp/drive.h"

namespace saeurekiste {

// The slot types, in the order the Type parameter lists them. A preset stores
// the *name*, so this list may grow at the end and may not be reordered.
//
// Distortion is called Saturator here because that is what it is: the drive
// stage's ten models with a tone control after them. Most of the rest came
// across unchanged; Guard is new, and is the note-triggered window Substrike
// keeps at the end of a lane rather than in its chain.
enum class SlotType : int {
   Off = 0,
   Saturator,
   Clipper,
   Wavefolder,
   Bitcrush,
   Filter,
   Eq,
   Compressor,
   Transient,
   Gate,
   Reverb,
   Delay,
   Warp,
   Smear,
   Ring,
   Stereo,
   Utility,
   Limiter,
   Comb,
   Guard,
};
constexpr int kNumSlotTypes = 20;

// Which part of the spectrum a slot processes. The rest passes by it.
enum class SlotBand : int {
   Full = 0,
   Low,
   Mid,
   High,
   LowMid,
   MidHigh,
};
constexpr int kNumSlotBands = 6;

// Where a slot sits, and the order the Position parameter lists them.
//
// Pre is between the oscillator and the ladder, Post is after the amplifier's
// own output stages, and Master is after the plugin's delay, the very end.
// Each bus is one stereo chain: Pre runs over the two unison paths as its two
// channels, and with one path the engine hands it the same signal twice and
// takes the mid of what comes back.
enum class ChainPosition : int {
   Pre = 0,
   Post,
   Master,
};
constexpr int kNumChainPositions = 3;

constexpr int kNumChainSlots = 16;
constexpr int kSlotValues = 6;

// One slot as the parameters set it. `v` holds A-F in the plain units the type
// gives them; what each one means is written beside each effect below and in
// params.cpp's shape table, which is what the window and the host are told.
struct SlotParams {
   SlotType type = SlotType::Off;
   SlotBand band = SlotBand::Full;
   double mix = 1.0;
   bool bypass = false;
   std::array<double, kSlotValues> v{};
};

// A choice (a model, a mode) jumps; everything else glides.
bool slotValueIsChoice(SlotType t, int i);
// The Clipper and the Wavefolder run inside the slot's 2x oversampler. The
// Saturator does not, because the drive stage already runs its models at twice
// the rate. Bitcrush does not either, unlike in Substrike: its aliasing is the
// effect, and the oversampler's downsampling filter would take part of it away.
bool slotIsOversampled(SlotType t);

class Effect {
public:
   virtual ~Effect() = default;
   virtual void prepare(double sampleRate) = 0;
   // Clears the state. Memory from attach() is cleared by the slot.
   virtual void reset() = 0;
   // The six values, in plain units. Called every 16 samples; an effect that
   // derives something expensive checks for a change itself.
   virtual void set(const double *v) = 0;
   // The note started (the engine's note-on, or a sequencer step).
   virtual void hit() {}
   virtual void process(float *l, float *r, int n) = 0;

   // Delay memory, in floats, at this rate. A slot runs one type at a time, so
   // every type of a slot shares one block, allocated with the rate and handed
   // over with attach() after prepare().
   virtual size_t memory(double /*sampleRate*/) const { return 0; }
   virtual void attach(float * /*mem*/) {}
   // How long the output may stay silent while the effect still holds a
   // sound (the gap before an echo), in samples.
   virtual int silentHold() const { return 0; }
   // How long the effect can go on sounding after its input stops, in
   // seconds, for the values `v` -- for the host's tail. Not the state's: the
   // settings', so it can be asked of a slot that has not run yet.
   virtual double tailSeconds(const double * /*v*/, double /*bpm*/) const { return 0.0; }
   // Beats per minute, for the synced times. Set before set().
   virtual void setTempo(double /*bpm*/) {}
};

// --------------------------------------------------------------- Saturator

// A: model (index), B: drive %, C: bias %, D: tone Hz (a lowpass after the
// stage), E: output dB. This plugin's own drive stage, then a DC blocker,
// since several models leave an offset.
class SaturatorFx : public Effect {
public:
   void prepare(double sampleRate) override;
   void reset() override;
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;

private:
   // How many set() calls a re-derivation of the drive stage waits for while
   // Drive or Bias is gliding: every 128 samples rather than every 16. The
   // stage measures its curve to match the level, which is far too much work to
   // repeat at the glide's own rate, and the last value of a glide is always
   // derived exactly.
   static constexpr int kDeriveEvery = 8;

   DriveStage stage_[2];
   ChainSvf tone_[2];
   ChainOnePoleHp dc_[2];
   double rate_ = 48000.0;
   std::array<double, 4> last_{-1.0, -1.0, -1.0, -1.0};  // what was derived
   std::array<double, 3> seen_{-1.0, -1.0, -1.0};        // the previous call's
   int wait_ = 0;
   double out_ = 1.0;
};

// ---------------------------------------------------------------- Clipper

// A: drive dB, B: knee % (hard to soft), C: ceiling dB.
class ClipperFx : public Effect {
public:
   static double shape(double u, double knee);

   void prepare(double) override {}
   void reset() override {}
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;

private:
   double gain_ = 1.0, knee_ = 0.0, ceiling_ = 1.0;
};

// ------------------------------------------------------------- Wavefolder

// A: drive dB, B: bias %, C: shape % (sine fold to triangle fold), D: output dB.
class WavefolderFx : public Effect {
public:
   void prepare(double sampleRate) override;
   void reset() override;
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;

private:
   ChainOnePoleHp dc_[2];
   double gain_ = 1.0, bias_ = 0.0, shape_ = 0.0, out_ = 1.0;
};

// ---------------------------------------------------------------- Bitcrush

// A: bits (1-16, fractional), B: sample-and-hold rate Hz (off at the top of
// its range), C: output dB. Rounds to the nearest step, so silence stays
// silent.
class BitcrushFx : public Effect {
public:
   static constexpr double kRateOff = 47990.0;

   void prepare(double sampleRate) override;
   void reset() override;
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;

private:
   double rate_ = 48000.0;
   double step_ = 1.0 / 128.0, hold_ = 0.0, out_ = 1.0;
   bool holding_ = false;
   double phase_ = 1.0;
   float held_[2] = {0.0f, 0.0f};
};

// ------------------------------------------------------------------ Filter

// A: mode (LP 12, LP 24, HP 12, HP 24, Band Pass, Notch, Peak), B: cutoff Hz,
// C: resonance %, D: envelope octaves, E: envelope decay ms, F: gain dB -- the
// level after the filter, or the gain of the bell in Peak mode, where the two
// are the same thing.
// The envelope starts at every note and falls to -60 dB over its decay, so a
// filter in front of the ladder is swept by the note the way the ladder is.
class FilterFx : public Effect {
public:
   void prepare(double sampleRate) override;
   void reset() override;
   void set(const double *v) override;
   void hit() override { env_ = 1.0; }
   void process(float *l, float *r, int n) override;

private:
   void setup(double cutoff);

   ChainSvf a_[2], b_[2];
   double rate_ = 48000.0;
   int mode_ = 1;
   double cutoff_ = 2000.0, q_ = 0.5, envOct_ = 0.0, envFall_ = 0.99, peak_ = 1.0,
          out_ = 1.0;
   double env_ = 0.0;
   double setFor_ = -1.0;
};

// ---------------------------------------------------------------------- EQ

// A: low shelf dB (100 Hz), B: mid dB, C: mid Hz, D: mid Q, E: high shelf dB
// (6 kHz), F: tilt dB (around 1 kHz, the high side up by half of it and the
// low side down by half).
class EqFx : public Effect {
public:
   void prepare(double sampleRate) override;
   void reset() override;
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;

private:
   static constexpr int kBands = 5;
   ChainBiquad bq_[2][kBands];
   double rate_ = 48000.0;
   std::array<double, 6> last_{1e9, 1e9, 1e9, 1e9, 1e9, 1e9};
};

// -------------------------------------------------------------------- Comb

// A comb of bells: A: start Hz (the first band), B: spacing Hz (from one band
// to the next), C: bands (1-32, how far the comb reaches), D: gain dB (below 0
// notches, above 0 peaks), E: width % (every band is this share of the spacing
// wide, in Hz, so they stay apart all the way up), F: taper % (+100 fades the
// gain out towards the last band, -100 fades it in from the first). With the
// start and the spacing on a note's pitch, the bands sit on its harmonics.
// Bands above 0.45 x the rate are left out.
class CombFx : public Effect {
public:
   static constexpr int kMaxBands = 32;
   void prepare(double sampleRate) override;
   void reset() override;
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;

private:
   ChainBiquad bq_[2][kMaxBands];
   int bands_ = 0;
   double rate_ = 48000.0;
   std::array<double, 6> last_{1e9, 1e9, 1e9, 1e9, 1e9, 1e9};
};

// -------------------------------------------------------------- Compressor

// A: threshold dB, B: ratio, C: attack ms, D: release ms, E: knee dB,
// F: makeup dB. Feed-forward, peak detection, the channels linked.
class CompressorFx : public Effect {
public:
   void prepare(double sampleRate) override;
   void reset() override { env_ = 0.0; }
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;

private:
   double rate_ = 48000.0;
   double thr_ = -18.0, slope_ = 0.75, knee_ = 6.0, makeup_ = 0.0, att_ = 0.0, rel_ = 0.0;
   double env_ = 0.0; // gain reduction, dB
};

// --------------------------------------------------------------- Transient

// A: attack %, B: sustain %, C: speed ms, D: output dB. Two pairs of
// followers: a fast and a slow attack (same release) find the onsets, a fast
// and a slow release (same attack) find the tail. Each contrast is taken up to
// 12 dB, and the percentages are a share of it.
class TransientFx : public Effect {
public:
   void prepare(double sampleRate) override;
   void reset() override;
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;

private:
   double rate_ = 48000.0;
   double attack_ = 0.0, sustain_ = 0.0, out_ = 1.0;
   double fastAtk_ = 0.0, slowAtk_ = 0.0, atkRel_ = 0.0, fastRel_ = 0.0, slowRel_ = 0.0,
          relAtk_ = 0.0;
   double eFastA_ = 0.0, eSlowA_ = 0.0, eFastR_ = 0.0, eSlowR_ = 0.0;
   double gainDb_ = 0.0, gainGlide_ = 1.0;
};

// -------------------------------------------------------------------- Gate

// A: mode (Gate, Hit), B: threshold dB, C: attack ms, D: hold ms, E: release
// ms, F: range dB (the bottom of it closes completely). Gate opens on level;
// Hit ignores the level and runs attack, hold and release from every note,
// which reshapes a smeared tail into one that ends where it is told to.
class GateFx : public Effect {
public:
   void prepare(double sampleRate) override;
   void reset() override;
   void set(const double *v) override;
   void hit() override;
   void process(float *l, float *r, int n) override;

private:
   double rate_ = 48000.0;
   bool hitMode_ = false;
   double thr_ = 0.01, attackStep_ = 1.0, release_ = 0.99, range_ = 0.0;
   int holdSamples_ = 0;
   double detector_ = 0.0, detRelease_ = 0.99;
   double g_ = 0.0;
   int hold_ = 0;
   bool opening_ = false;
};

// ------------------------------------------------------------------ Guard

// A: window ms (silence after the note), B: fade ms (a raised cosine opening
// it), C: depth % (how far down the window holds the signal).
//
// Substrike keeps this at the end of every lane rather than in the chain; here
// it is a slot, so it can be put in front of the saturator that would
// otherwise flatten the attack, or after a reverb to take its front off. With
// both times at zero it does nothing at all.
class GuardFx : public Effect {
public:
   void prepare(double sampleRate) override;
   void reset() override;
   void set(const double *v) override;
   void hit() override;
   void process(float *l, float *r, int n) override;

private:
   double rate_ = 48000.0;
   double window_ = 0.0, fade_ = 0.0, floor_ = 0.0;
   double pos_ = 1.0e12; // samples since the note; far away is open
};

// --------------------------------------------------------------- Limiter

// A: gain dB (into the limiter), B: ceiling dB, C: release ms. A peak limiter
// without lookahead, so it adds no latency: the gain drops at once to whatever
// keeps the peak at the ceiling, holds for 25 ms (a period of 40 Hz) and
// recovers over the release.
class LimiterFx : public Effect {
public:
   void prepare(double sampleRate) override;
   void reset() override {
      gain_ = 1.0;
      hold_ = 0;
   }
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;

private:
   double rate_ = 48000.0;
   double in_ = 1.0, ceiling_ = 1.0, release_ = 0.999;
   double gain_ = 1.0;
   int holdSamples_ = 1200, hold_ = 0;
};

// ------------------------------------------------------------ time effects

// A: time ms, B: sync (Free or a note value, which then sets the time from the
// tempo), C: feedback %, D: colour % (below 0 a lowpass in the feedback
// closing towards 400 Hz, above 0 a highpass opening towards 2 kHz), E: drive
// % (a soft clip in the feedback), F: ping-pong %. The output is the input
// with its echoes added, so the slot's Mix sets the level of the echoes and
// never takes anything away from the note.
class DelayFx : public Effect {
public:
   static constexpr double kMaxSeconds = 2.0;

   void prepare(double sampleRate) override;
   void reset() override;
   void setTempo(double bpm) override { tempo_ = bpm; }
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;
   size_t memory(double sampleRate) const override;
   void attach(float *mem) override;
   int silentHold() const override { return static_cast<int>(cur_) + hold_; }
   double tailSeconds(const double *v, double bpm) const override;

private:
   static int lineSize(double rate) { return static_cast<int>(kMaxSeconds * rate) + 8; }

   ChainDelayLine line_[2];
   ChainOnePoleLp lp_[2];
   ChainOnePoleHp hp_[2];
   double rate_ = 48000.0, tempo_ = 120.0;
   double target_ = 2.0, cur_ = 2.0, glide_ = 0.0;
   double fb_ = 0.0, drive_ = 0.0, pingPong_ = 0.0;
   bool lowPass_ = false, snap_ = true;
   int hold_ = 480;
   double lastColour_ = 1e9;
};

// A: time ms, B: sync, C: feedback %, D: pitch st, E: mode (Forward,
// Reverse, Taps), F: diffusion %. A delay whose repeats change: each pass
// through the feedback is shifted by Pitch, so the echoes climb or fall;
// Reverse plays every window of Time backwards; Taps reads four taps across
// Time, alternating left and right; Diffusion smears each repeat through four
// allpasses.
class WarpFx : public Effect {
public:
   void prepare(double sampleRate) override;
   void reset() override;
   void setTempo(double bpm) override { tempo_ = bpm; }
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;
   size_t memory(double sampleRate) const override;
   void attach(float *mem) override;
   int silentHold() const override;
   double tailSeconds(const double *v, double bpm) const override;

private:
   static constexpr int kDiffusers = 4;
   static int lineSize(double rate) { return static_cast<int>(DelayFx::kMaxSeconds * rate) + 8; }
   static int diffuserSize(double rate) { return static_cast<int>(0.02 * rate) + 4; }
   // Two reads of a line, half a window apart, crossfaded: a delay of `base`
   // whose pitch is shifted as the reads drift.
   double shifted(const ChainDelayLine &line, double base) const;

   ChainDelayLine line_[2];
   ChainAllpass diff_[2][kDiffusers];
   ChainOnePoleHp dc_[2];
   double rate_ = 48000.0, tempo_ = 120.0;
   double time_ = 2.0, target_ = 2.0, glide_ = 0.0;
   double fb_ = 0.0, ratio_ = 1.0, diffusion_ = 0.0;
   int mode_ = 0;
   bool snap_ = true;
   double phase_ = 0.0;
   double window_ = 1.0;
   std::array<int, kDiffusers> diffLen_{};
};

// A: size %, B: decay ms (to -60 dB), C: damping Hz, D: pre-delay ms, E: low
// cut Hz (before the reverb), F: width %. An eight-line feedback delay network
// with a Hadamard mix and four allpasses of input diffusion; small rooms and
// no modulation, for smearing a note into a room rather than for halls.
class ReverbFx : public Effect {
public:
   void prepare(double sampleRate) override;
   void reset() override;
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;
   size_t memory(double sampleRate) const override;
   void attach(float *mem) override;
   int silentHold() const override { return static_cast<int>(predelay_) + hold_; }
   double tailSeconds(const double *v, double bpm) const override;

private:
   static constexpr int kLines = 8;
   static constexpr int kDiffusers = 4;
   static constexpr double kMaxPredelayMs = 250.0;

   ChainDelayLine pre_[2];
   ChainAllpass diff_[2][kDiffusers];
   ChainDelayLine line_[kLines];
   ChainOnePoleLp damp_[kLines];
   ChainSvf lowCut_[2];
   double rate_ = 48000.0;
   std::array<int, kLines> len_{};
   std::array<double, kLines> gain_{};
   std::array<int, kDiffusers> diffLen_{};
   double predelay_ = 2.0;
   double width_ = 1.0;
   double lowCutHz_ = 20.0;
   bool lowCutOn_ = false;
   std::array<double, 6> last_{1e9, 1e9, 1e9, 1e9, 1e9, 1e9};
   int hold_ = 480;
};

// A: time ms (the stages' lengths added up), B: diffusion %, C: stages (4, 8,
// 12, 16), D: width %. A chain of allpasses: it spreads a note out in time
// without adding a tail, the way a long cable or a cheap spring smears a
// transient.
class SmearFx : public Effect {
public:
   void prepare(double sampleRate) override;
   void reset() override;
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;
   size_t memory(double sampleRate) const override;
   void attach(float *mem) override;

private:
   static constexpr int kStages = 16;
   static constexpr double kMaxMs = 80.0;
   static int stageSize(double rate) { return static_cast<int>(kMaxMs / 3.0 * 0.001 * rate) + 4; }

   ChainAllpass ap_[2][kStages];
   double rate_ = 48000.0;
   std::array<std::array<int, kStages>, 2> len_{};
   double g_ = 0.5;
   int stages_ = 8;
};

// A: mode (Ring, Shift Up, Shift Down), B: frequency Hz, C: envelope oct,
// D: envelope decay ms. Ring multiplies by a cosine; the shifts move every
// partial by the frequency, through a Hilbert pair. The oscillator restarts on
// every note at its peak, so every note sounds the same.
class RingFx : public Effect {
public:
   void prepare(double sampleRate) override;
   void reset() override;
   void set(const double *v) override;
   void hit() override;
   void process(float *l, float *r, int n) override;

private:
   double rate_ = 48000.0;
   int mode_ = 0;
   double freq_ = 100.0, envOct_ = 0.0, envFall_ = 0.99, env_ = 0.0;
   double phase_ = 0.0;
   // O. Niemitalo's 90-degree phase difference network.
   std::array<std::array<std::array<double, 4>, 4>, 2> ha_{}, hb_{};
   std::array<double, 2> delayed_{};
};

// A: width % (0 mono, 100 as it is, 200 the sides doubled), B: Haas ms (a
// short delay on one side: above 0 the right, below 0 the left).
class StereoFx : public Effect {
public:
   static constexpr double kMaxHaasMs = 30.0;

   void prepare(double sampleRate) override;
   void reset() override;
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;
   size_t memory(double sampleRate) const override;
   void attach(float *mem) override;

private:
   static int lineSize(double rate) {
      return static_cast<int>(kMaxHaasMs * 0.001 * rate) + 8;
   }
   ChainDelayLine line_[2];
   double rate_ = 48000.0;
   double width_ = 1.0, haas_ = 0.0;
};

// A: gain dB (the bottom of it is silence), B: polarity (Normal, Invert),
// C: channels (Stereo, Mono, Swap, Left, Right).
class UtilityFx : public Effect {
public:
   void prepare(double) override {}
   void reset() override {}
   void set(const double *v) override;
   void process(float *l, float *r, int n) override;

private:
   double gain_ = 1.0;
   int channels_ = 0;
};

// Where a slot is: the chain's crossovers and the tempo.
struct SlotEnv {
   double xoverLow = 200.0, xoverHigh = 2500.0;
   double tempo = 120.0;
};

// One effect slot: the type's effect, the band split around it, the
// oversampling for the nonlinearities, the mix, and the glide of every value.
class Slot {
public:
   static constexpr int kStep = 16; // values glide in steps of this many samples

   // Allocates the slot's delay memory for the rate, so it belongs on the main
   // thread, in activate().
   void prepare(double sampleRate);
   // Clears every state; the next call snaps to its parameters.
   void reset();
   // Whether the slot is doing anything (Off and fully bypassed are not).
   bool live() const { return live_; }
   int silentHold() const;
   // The tail of whatever `p` sets the slot to; see Effect::tailSeconds().
   double tailSeconds(const SlotParams &p, double bpm);
   // Processes n samples in place. `hits` are offsets into the n samples, in
   // order, at which a note started.
   void process(float *l, float *r, int n, const SlotParams &p, const SlotEnv &e,
                const int *hits, int hitCount);

private:
   Effect &effect(SlotType t);
   void wake(const SlotParams &p, const SlotEnv &e);
   void glide(const SlotParams &p, const SlotEnv &e, bool snap);
   void apply();
   void segment(float *l, float *r, int n, double target);

   SaturatorFx saturator_;
   ClipperFx clipper_;
   WavefolderFx wavefolder_;
   BitcrushFx bitcrush_;
   FilterFx filter_;
   EqFx eq_;
   CompressorFx compressor_;
   TransientFx transient_;
   GateFx gate_;
   ReverbFx reverb_;
   DelayFx delay_;
   WarpFx warp_;
   SmearFx smear_;
   RingFx ring_;
   StereoFx stereo_;
   UtilityFx utility_;
   LimiterFx limiter_;
   CombFx comb_;
   GuardFx guard_;

   // The delay memory every type of this slot shares.
   void clearMemory();
   std::vector<float> memory_;
   bool memoryDirty_ = false;

   static constexpr int kOversampling = 2;
   ChainOversampler osL_, osR_;
   ChainCrossover3 xL_, xR_;
   double rate_ = 48000.0;
   double stepCoef_ = 0.0, mixCoef_ = 0.0;

   SlotType type_ = SlotType::Off;
   SlotBand band_ = SlotBand::Full;
   std::array<double, kSlotValues> cur_{};
   double xoverLow_ = 0.0, xoverHigh_ = 0.0, xoverSetLow_ = -1.0, xoverSetHigh_ = -1.0;
   double mix_ = 0.0;
   int counter_ = 0;
   bool primed_ = false;
   bool live_ = false;

   static constexpr int kMaxUp = kStep * kOversampling;
   std::array<float, kStep> dryL_{}, dryR_{}, selL_{}, selR_{}, wetL_{}, wetR_{}, restL_{},
      restR_{};
   std::array<float, kMaxUp> upL_{}, upR_{};
};

// Every slot and where each of them sits. Slots are applied in index order, so
// the index *is* the position in the chain: the window's ADD, remove and move
// keep the used slots packed at the front rather than giving them an order
// parameter of their own.
struct ChainParams {
   std::array<SlotParams, kNumChainSlots> slots{};
   std::array<ChainPosition, kNumChainSlots> position{};
   double xoverLow = 200.0;
   double xoverHigh = 2500.0;
};

class Chain {
public:
   void prepare(double sampleRate);
   void reset();
   // Hands over where every slot sits and what it is set to. The effects
   // themselves are fed by process() so a slot that is not live costs nothing.
   void setParams(const ChainParams &p);
   // The host's tempo, for the synced delay times.
   void setTempo(double bpm) { env_.tempo = bpm > 0.0 ? bpm : 120.0; }
   // Runs the slots whose Position is `position`, in slot order, in place.
   void process(float *l, float *r, int n, ChainPosition position, const int *hits,
                int hitCount);
   // Whether this bus has to be processed at all: a slot whose type is not Off,
   // or one still fading out after being switched off. This is deliberately not
   // "is a slot live" -- a slot that has never run is not live yet, and the
   // note that switches it on is the one that has to wake it.
   bool needs(ChainPosition position) const;
   // The longest a live slot may stay silent while it still holds a sound --
   // a delay waiting for its next echo -- in samples. The engine waits this
   // long past the last sound before it lets the host put the plugin to sleep.
   int silentHold() const;
   // The longest tail any slot that is switched on can hold, in seconds.
   double tailSeconds();

private:
   std::array<Slot, kNumChainSlots> slots_{};
   std::array<SlotParams, kNumChainSlots> params_{};
   std::array<ChainPosition, kNumChainSlots> pos_{};
   SlotEnv env_{};
};

// A synced time as a note value: the choices of a Sync letter, and their
// length in quarter notes (0 for Free, which leaves the time in ms).
double syncBeats(int choice);

} // namespace saeurekiste
