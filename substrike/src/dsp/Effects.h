#pragma once

#include <array>

#include "dsp/Crossover.h"
#include "dsp/Drive.h"
#include "dsp/OnePole.h"
#include "dsp/Oversampler.h"
#include "dsp/Svf.h"

namespace substrike::dsp {

// The slot types, in the order the Type parameter lists them. State stores the
// label, so the list may grow at the end.
enum class SlotType : int
{
    Off = 0,
    Distortion,
    Clipper,
    Wavefolder,
    Bitcrush,
    Filter,
    Eq,
    Compressor,
    Transient,
    Gate,
};
constexpr int kNumSlotTypes = 10;

// Which part of the spectrum a slot processes. The rest passes by it.
enum class Band : int
{
    Full = 0,
    Low,
    Mid,
    High,
    LowMid,
    MidHigh,
};

constexpr int kNumSlots = 6;
constexpr int kSlotValues = 6;

// One slot as the parameters set it. `v` holds A-F in the plain units the
// type gives them (see the shape table in plugin/Params.cpp); what each one
// means is written beside each effect below.
struct SlotParams
{
    SlotType type = SlotType::Off;
    Band band = Band::Full;
    double mix = 1.0;
    bool bypass = false;
    std::array<double, kSlotValues> v{};
};

// A choice (a model, a mode) jumps; everything else glides.
bool slotValueIsChoice(SlotType t, int i);
// The drive group runs inside the oversampler.
bool slotIsOversampled(SlotType t);

class Effect
{
public:
    virtual ~Effect() = default;
    virtual void prepare(double sampleRate) = 0;
    virtual void reset() = 0;
    // The six values, in plain units. Called every 16 samples; an effect
    // that derives something expensive checks for a change itself.
    virtual void set(const double* v) = 0;
    // The lane fired a hit (the master: a note arrived).
    virtual void hit() {}
    virtual void process(float* l, float* r, int n) = 0;
};

// A: model (index), B: drive %, C: bias %, D: tone Hz (lowpass after the
// stage), E: output dB. SaeureKiste's drive stage, then a DC blocker, since
// several models leave an offset.
class DistortionFx : public Effect
{
public:
    void prepare(double sampleRate) override;
    void reset() override;
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;

private:
    DriveStage stage_[2];
    Svf tone_[2];
    OnePoleHp dc_[2];
    double rate_ = 48000.0;
    std::array<double, 5> last_{-1.0, -1.0, -1.0, -1.0, -1.0};
    double out_ = 1.0;
};

// A: drive dB, B: knee % (hard to soft), C: ceiling dB.
class ClipperFx : public Effect
{
public:
    static double shape(double u, double knee);

    void prepare(double) override {}
    void reset() override {}
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;

private:
    double gain_ = 1.0, knee_ = 0.0, ceiling_ = 1.0;
};

// A: drive dB, B: bias %, C: shape % (sine fold to triangle fold), D: output dB.
class WavefolderFx : public Effect
{
public:
    void prepare(double sampleRate) override;
    void reset() override;
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;

private:
    OnePoleHp dc_[2];
    double gain_ = 1.0, bias_ = 0.0, shape_ = 0.0, out_ = 1.0;
};

// A: bits (1-16, fractional), B: sample-and-hold rate Hz (off at the top of
// its range), C: output dB. Rounds to the nearest step, so silence stays
// silent.
class BitcrushFx : public Effect
{
public:
    static constexpr double kRateOff = 47990.0;

    void prepare(double sampleRate) override;
    void reset() override;
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;

private:
    double rate_ = 48000.0;
    double step_ = 1.0 / 128.0, hold_ = 0.0, out_ = 1.0;
    bool holding_ = false;
    double phase_ = 1.0;
    float held_[2] = {0.0f, 0.0f};
};

// A: mode (LP 12, LP 24, HP 12, HP 24, Band Pass, Notch, Peak), B: cutoff Hz,
// C: resonance %, D: envelope octaves, E: envelope decay ms, F: peak gain dB.
// The envelope starts at every hit and falls to -60 dB over its decay.
class FilterFx : public Effect
{
public:
    void prepare(double sampleRate) override;
    void reset() override;
    void set(const double* v) override;
    void hit() override { env_ = 1.0; }
    void process(float* l, float* r, int n) override;

private:
    void setup(double cutoff);

    Svf a_[2], b_[2];
    double rate_ = 48000.0;
    int mode_ = 1;
    double cutoff_ = 2000.0, q_ = 0.5, envOct_ = 0.0, envFall_ = 0.99, peak_ = 1.0;
    double env_ = 0.0;
    double setFor_ = -1.0;
};

// A biquad, transposed direct form II.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0.0; }
    double tick(double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    // R. Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter
    // coefficients".
    void lowShelf(double f, double db, double q, double rate);
    void highShelf(double f, double db, double q, double rate);
    void peak(double f, double db, double q, double rate);
};

// A: low shelf dB (100 Hz), B: mid dB, C: mid Hz, D: mid Q, E: high shelf dB
// (6 kHz), F: tilt dB (around 1 kHz, the high side up by half of it and the
// low side down by half).
class EqFx : public Effect
{
public:
    void prepare(double sampleRate) override;
    void reset() override;
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;

private:
    static constexpr int kBands = 5;
    Biquad bq_[2][kBands];
    double rate_ = 48000.0;
    std::array<double, 6> last_{1e9, 1e9, 1e9, 1e9, 1e9, 1e9};
};

// A: threshold dB, B: ratio, C: attack ms, D: release ms, E: knee dB,
// F: makeup dB. Feed-forward, peak detection, the channels linked.
class CompressorFx : public Effect
{
public:
    void prepare(double sampleRate) override;
    void reset() override { env_ = 0.0; }
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;

private:
    double rate_ = 48000.0;
    double thr_ = -18.0, slope_ = 0.75, knee_ = 6.0, makeup_ = 0.0, att_ = 0.0, rel_ = 0.0;
    double env_ = 0.0; // gain reduction, dB
};

// A: attack %, B: sustain %, C: speed ms, D: output dB. Two pairs of
// followers: a fast and a slow attack (same release) find the onsets, a fast
// and a slow release (same attack) find the tail. Each percent is a decibel
// per decibel of difference, so 100 % doubles the contrast.
class TransientFx : public Effect
{
public:
    void prepare(double sampleRate) override;
    void reset() override;
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;

private:
    double rate_ = 48000.0;
    double attack_ = 0.0, sustain_ = 0.0, out_ = 1.0;
    double fastAtk_ = 0.0, slowAtk_ = 0.0, atkRel_ = 0.0, fastRel_ = 0.0, slowRel_ = 0.0, relAtk_ = 0.0;
    double eFastA_ = 0.0, eSlowA_ = 0.0, eFastR_ = 0.0, eSlowR_ = 0.0;
};

// A: mode (Gate, Hit), B: threshold dB, C: attack ms, D: hold ms, E: release
// ms, F: range dB (the bottom of it closes completely). Gate opens on level;
// Hit ignores the level and runs attack, hold and release from every hit,
// which reshapes a smeared tail into one that ends where it is told to.
class GateFx : public Effect
{
public:
    void prepare(double sampleRate) override;
    void reset() override;
    void set(const double* v) override;
    void hit() override;
    void process(float* l, float* r, int n) override;

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

// Where a slot is: the lane's (or the master's) crossovers and the rate.
struct SlotEnv
{
    double sampleRate = 48000.0;
    double xoverLow = 150.0, xoverHigh = 2500.0;
};

// One effect slot: the type's effect, the band split around it, the
// oversampling for the drive group, the mix, and the glide of every value.
class Slot
{
public:
    static constexpr int kStep = 16; // values glide in steps of this many samples

    void prepare(double sampleRate, int oversampling);
    // Clears every state; the next call snaps to its parameters.
    void reset();
    // Whether the slot is doing anything (Off and fully bypassed are not).
    bool live() const { return live_; }
    // Processes n samples in place. `hits` are offsets into the n samples, in
    // order, at which the lane fired.
    void process(float* l, float* r, int n, const SlotParams& p, const SlotEnv& e, const int* hits, int hitCount);

private:
    Effect& effect(SlotType t);
    void wake(const SlotParams& p, const SlotEnv& e);
    void glide(const SlotParams& p, const SlotEnv& e, bool snap);
    void segment(float* l, float* r, int n, double target);

    DistortionFx distortion_;
    ClipperFx clipper_;
    WavefolderFx wavefolder_;
    BitcrushFx bitcrush_;
    FilterFx filter_;
    EqFx eq_;
    CompressorFx compressor_;
    TransientFx transient_;
    GateFx gate_;

    Oversampler osL_, osR_;
    Crossover3 xL_, xR_;
    double rate_ = 48000.0;
    int factor_ = 1;
    double stepCoef_ = 0.0, mixCoef_ = 0.0;

    SlotType type_ = SlotType::Off;
    Band band_ = Band::Full;
    std::array<double, kSlotValues> cur_{};
    double xoverLow_ = 0.0, xoverHigh_ = 0.0, xoverSetLow_ = -1.0, xoverSetHigh_ = -1.0;
    double mix_ = 0.0;
    int counter_ = 0;
    bool primed_ = false;
    bool live_ = false;

    static constexpr int kMaxUp = kStep * Oversampler::kMaxFactor;
    std::array<float, kStep> dryL_{}, dryR_{}, selL_{}, selR_{}, wetL_{}, wetR_{}, restL_{}, restR_{};
    std::array<float, kMaxUp> upL_{}, upR_{};
};

} // namespace substrike::dsp
