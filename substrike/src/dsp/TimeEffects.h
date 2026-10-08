#pragma once

#include <array>

#include "dsp/Effect.h"
#include "dsp/OnePole.h"
#include "dsp/Svf.h"

namespace substrike::dsp {

// A delay line over memory the slot owns. read() before push(): read(d) is
// the sample pushed d samples ago, so read(1) is the last one.
struct DelayLine
{
    float* buf = nullptr;
    int size = 0;
    int w = 0;

    void attach(float* mem, int n)
    {
        buf = mem;
        size = n;
        w = 0;
    }
    void reset() { w = 0; }
    void push(float x)
    {
        buf[w] = x;
        if (++w == size)
            w = 0;
    }
    float at(int d) const
    {
        int i = w - d;
        if (i < 0)
            i += size;
        return buf[i];
    }
    // A fractional delay of at least 2 samples, four-point Hermite.
    double read(double d) const;
};

// A Schroeder allpass on a delay line. The length is whole samples: an
// interpolated read inside the loop would be a lowpass, and the allpass
// would no longer pass everything.
struct Allpass
{
    DelayLine line;
    double tick(double x, int length, double g)
    {
        const double d = line.at(length);
        const double v = x + g * d;
        line.push(static_cast<float>(v));
        return d - g * v;
    }
};

// A: time ms, B: sync (Free or a note value, which then sets the time from
// the tempo), C: feedback %, D: colour % (below 0 a lowpass in the feedback
// closing towards 400 Hz, above 0 a highpass opening towards 2 kHz), E: drive
// % (a soft clip in the feedback), F: ping-pong %. The output is the input
// with its echoes added, so the slot's Mix sets the level of the echoes and
// never takes anything away from the hit.
class DelayFx : public Effect
{
public:
    static constexpr double kMaxSeconds = 2.0;

    void prepare(double sampleRate) override;
    void reset() override;
    void setTempo(double bpm) override { tempo_ = bpm; }
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;
    size_t memory(double sampleRate) const override;
    void attach(float* mem) override;
    int silentHold() const override { return static_cast<int>(cur_) + hold_; }

private:
    static int lineSize(double rate) { return static_cast<int>(kMaxSeconds * rate) + 8; }

    DelayLine line_[2];
    OnePoleLp lp_[2];
    OnePoleHp hp_[2];
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
// allpasses. Like Delay, the output is the input with its echoes added.
class WarpFx : public Effect
{
public:
    void prepare(double sampleRate) override;
    void reset() override;
    void setTempo(double bpm) override { tempo_ = bpm; }
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;
    size_t memory(double sampleRate) const override;
    void attach(float* mem) override;
    int silentHold() const override;

private:
    static constexpr int kDiffusers = 4;
    static int lineSize(double rate) { return static_cast<int>(DelayFx::kMaxSeconds * rate) + 8; }
    static int diffuserSize(double rate) { return static_cast<int>(0.02 * rate) + 4; }
    // Two reads of a line, half a window apart, crossfaded: a delay of
    // `base` whose pitch is shifted as the reads drift.
    double shifted(const DelayLine& line, double base) const;

    DelayLine line_[2];
    Allpass diff_[2][kDiffusers];
    OnePoleHp dc_[2];
    double rate_ = 48000.0, tempo_ = 120.0;
    double time_ = 2.0, target_ = 2.0, glide_ = 0.0;
    double fb_ = 0.0, ratio_ = 1.0, diffusion_ = 0.0;
    int mode_ = 0;
    bool snap_ = true;
    double phase_ = 0.0; // the shifter's or the reverser's window, 0..1
    double window_ = 1.0;
    std::array<int, kDiffusers> diffLen_{};
};

// A: size %, B: decay ms (to -60 dB), C: damping Hz, D: pre-delay ms, E: low
// cut Hz (before the reverb), F: width %. An eight-line feedback delay network
// with a Hadamard mix and four allpasses of input diffusion; small rooms and
// no modulation, for smearing a hit into a room rather than for halls. The
// output is the reverb alone.
class ReverbFx : public Effect
{
public:
    void prepare(double sampleRate) override;
    void reset() override;
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;
    size_t memory(double sampleRate) const override;
    void attach(float* mem) override;
    int silentHold() const override { return static_cast<int>(predelay_) + hold_; }

private:
    static constexpr int kLines = 8;
    static constexpr int kDiffusers = 4;
    static constexpr double kMaxPredelayMs = 250.0;
    void layout(const double* sizes, const double* diffSizes, int* lines, int* diffs, int* pre) const;

    DelayLine pre_[2];
    Allpass diff_[2][kDiffusers];
    DelayLine line_[kLines];
    OnePoleLp damp_[kLines];
    Svf lowCut_[2];
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

// A: time ms (the stages' lengths added up), B: diffusion %, C: stages (4,
// 8, 12, 16), D: width %. A chain of allpasses: it spreads a hit out in time without
// adding a tail, the way a long cable or a cheap spring smears a transient.
class SmearFx : public Effect
{
public:
    void prepare(double sampleRate) override;
    void reset() override;
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;
    size_t memory(double sampleRate) const override;
    void attach(float* mem) override;

private:
    static constexpr int kStages = 16;
    static constexpr double kMaxMs = 80.0;
    // The longest stage is under a third of Time, whatever the count.
    static int stageSize(double rate) { return static_cast<int>(kMaxMs / 3.0 * 0.001 * rate) + 4; }

    Allpass ap_[2][kStages];
    double rate_ = 48000.0;
    std::array<std::array<int, kStages>, 2> len_{};
    double g_ = 0.5;
    int stages_ = 8;
};

// A: mode (Ring, Shift Up, Shift Down), B: frequency Hz, C: envelope oct,
// D: envelope decay ms. Ring multiplies by a cosine; the shifts move every
// partial by the frequency, through a Hilbert pair, so a kick's harmonics
// stop being harmonic. The oscillator restarts on every hit at its peak, so
// every hit sounds the same; the envelope starts there too and falls to
// -60 dB over its decay.
class RingFx : public Effect
{
public:
    void prepare(double sampleRate) override;
    void reset() override;
    void set(const double* v) override;
    void hit() override;
    void process(float* l, float* r, int n) override;

private:
    double rate_ = 48000.0;
    int mode_ = 0;
    double freq_ = 100.0, envOct_ = 0.0, envFall_ = 0.99, env_ = 0.0;
    double phase_ = 0.0;
    // Olli Niemitalo's 90-degree phase difference network: per channel two
    // chains of four allpasses in z^-2, each stage holding x[n-1], x[n-2],
    // y[n-1] and y[n-2]; the first chain's output one sample late.
    std::array<std::array<std::array<double, 4>, 4>, 2> ha_{}, hb_{};
    std::array<double, 2> delayed_{};
};

// A: width % (0 mono, 100 as it is, 200 the sides doubled), B: Haas ms (a
// short delay on one side: above 0 the right, below 0 the left).
class StereoFx : public Effect
{
public:
    static constexpr double kMaxHaasMs = 30.0;

    void prepare(double sampleRate) override;
    void reset() override;
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;
    size_t memory(double sampleRate) const override;
    void attach(float* mem) override;

private:
    static int lineSize(double rate) { return static_cast<int>(kMaxHaasMs * 0.001 * rate) + 8; }
    DelayLine line_[2];
    double rate_ = 48000.0;
    double width_ = 1.0, haas_ = 0.0;
};

// A: gain dB (the bottom of it is silence), B: polarity (Normal, Invert),
// C: channels (Stereo, Mono, Swap, Left, Right).
class UtilityFx : public Effect
{
public:
    void prepare(double) override {}
    void reset() override {}
    void set(const double* v) override;
    void process(float* l, float* r, int n) override;

private:
    double gain_ = 1.0;
    int channels_ = 0;
};

} // namespace substrike::dsp
