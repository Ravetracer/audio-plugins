#pragma once

#include <array>
#include <vector>

#include "dsp/Crossover.h"
#include "dsp/Drive.h"
#include "dsp/Effect.h"
#include "dsp/OnePole.h"
#include "dsp/Oversampler.h"
#include "dsp/Svf.h"
#include "dsp/TimeEffects.h"

namespace substrike::dsp {

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

// A comb of bells: A: start Hz (the first band), B: spacing Hz (from one
// band to the next), C: bands (1-32, how far the comb reaches), D: gain dB
// (below 0 notches, above 0 peaks), E: width % (every band is this share of
// the spacing wide, in Hz, so they stay apart all the way up), F: taper %
// (+100 fades the gain out towards the last band, -100 fades it in from the
// first). With the
// start and the spacing on a kick's pitch, the bands sit on its harmonics.
// Bands above 0.45 x the rate are left out.
class CombFx : public Effect
{
public:
    static constexpr int kMaxBands = 32;
    void prepare(double sampleRate) override;
    void reset() override;
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;

private:
    Biquad bq_[2][kMaxBands];
    int bands_ = 0;
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
// and a slow release (same attack) find the tail. Each contrast is taken up
// to 12 dB, and the percentages are a share of it: +100 % lifts an onset by
// up to 12 dB, -100 % cuts a tail by as much. The gain glides over 0.3 ms.
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
    double gainDb_ = 0.0, gainGlide_ = 1.0;
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

// A: gain dB (into the limiter), B: ceiling dB, C: release ms. A peak
// limiter without lookahead, so it adds no latency: the gain drops at once
// to whatever keeps the peak at the ceiling, holds for 25 ms (a period of
// 40 Hz, so it does not follow the waveform of a low kick) and recovers over
// the release. It runs at the base rate, where the ceiling is exact: an
// oversampler's downsampling filter would overshoot it.
class LimiterFx : public Effect
{
public:
    void prepare(double sampleRate) override;
    void reset() override
    {
        gain_ = 1.0;
        hold_ = 0;
    }
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;
    // For the master's Output Clip: the limiter at a fixed ceiling.
    void setFixed(double ceiling, double releaseMs);

private:
    double rate_ = 48000.0;
    double in_ = 1.0, ceiling_ = 1.0, release_ = 0.999;
    double gain_ = 1.0;
    int holdSamples_ = 1200, hold_ = 0;
};

// Where a slot is: the lane's (or the master's) crossovers and the rate.
struct SlotEnv
{
    double sampleRate = 48000.0;
    double xoverLow = 150.0, xoverHigh = 2500.0;
    double tempo = 120.0;
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
    // How long the slot's output may stay silent while it still holds a
    // sound, in samples (see Effect::silentHold).
    int silentHold() const;
    // Processes n samples in place. `hits` are offsets into the n samples, in
    // order, at which the lane fired. `keyRatios`, when given, holds one
    // frequency ratio per hit: the tuned letters (a Filter's cutoff, an EQ's
    // mid frequency, a Ring Mod's frequency) are scaled by it from that hit
    // on, so a chain can follow the note the way a Body does.
    void process(float* l, float* r, int n, const SlotParams& p, const SlotEnv& e, const int* hits, int hitCount,
                 const double* keyRatios = nullptr);

private:
    Effect& effect(SlotType t);
    void wake(const SlotParams& p, const SlotEnv& e);
    void glide(const SlotParams& p, const SlotEnv& e, bool snap);
    // Hands the current values to the effect, the tuned letters scaled by
    // the key ratio.
    void apply();
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
    ReverbFx reverb_;
    DelayFx delay_;
    WarpFx warp_;
    SmearFx smear_;
    RingFx ring_;
    StereoFx stereo_;
    UtilityFx utility_;
    LimiterFx limiter_;
    CombFx comb_;

    // The delay memory every type of this slot shares; see Effect::memory.
    void clearMemory();
    std::vector<float> memory_;
    bool memoryDirty_ = false;

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
    double keyRatio_ = 1.0;
    int counter_ = 0;
    bool primed_ = false;
    bool live_ = false;

    static constexpr int kMaxUp = kStep * Oversampler::kMaxFactor;
    std::array<float, kStep> dryL_{}, dryR_{}, selL_{}, selR_{}, wetL_{}, wetR_{}, restL_{}, restR_{};
    std::array<float, kMaxUp> upL_{}, upR_{};
};

} // namespace substrike::dsp
