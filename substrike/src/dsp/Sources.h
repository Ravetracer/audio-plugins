#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "dsp/Curve.h"
#include "dsp/Random.h"
#include "dsp/Svf.h"

namespace substrike::dsp {

// ------------------------------------------------------------------ parameters

enum class Wave : int
{
    Sine = 0,
    Triangle,
    Saw,
    Square,
    Additive,
};

// The tonal oscillator. Its pitch falls from pitchStart to pitchEnd along the
// pitch curve within sweepMs; its level runs attack, hold and then the amp
// curve within decayMs. Curvatures are -1..1 and bend the drawn curves.
struct BodyParams
{
    double pitchStart = 350.0; // Hz
    double pitchEnd = 48.0;    // Hz
    double sweepMs = 120.0;
    double sweepCurve = 0.55;
    double keyTrack = 0.0; // 0..1: share of the note's distance from the root
    double attackMs = 0.0;
    double holdMs = 30.0;
    double decayMs = 450.0;
    double decayCurve = 0.45;
    double phase = 0.0; // start phase in turns, latched per hit

    Wave wave = Wave::Sine;
    double shape = 0.0;   // 0..1: tanh saturation of the oscillator
    double tilt = 0.0;    // -1..1: additive spectrum, dark to flat
    double even = 1.0;    // 0..1: level of the even partials (additive)
    double stretch = 0.0; // 0..1: inharmonicity of the partials (additive)
    double fmAmount = 0.0; // 0..1
    double fmRatio = 2.0;  // modulator frequency / carrier frequency
    double fmDecayMs = 30.0;
    double feedback = 0.0; // 0..1: phase feedback of the oscillator onto itself
    double drift = 0.0;    // 0..1: slow random pitch wander within a hit
};

enum class ClickType : int
{
    Impulse = 0,
    Noise,
    Blip,
    Zap,
};

// A transient, rendered whole into a buffer when the hit fires and
// peak-normalised, so the filter settings do not change its level.
struct ClickParams
{
    ClickType type = ClickType::Noise;
    double decayMs = 6.0; // to -60 dB
    FilterMode filter = FilterMode::HighPass;
    double cutoff = 2500.0;
    double reso = 0.1;     // 0..1
    double pitch = 1500.0; // Blip and Zap
    double sweepOct = 3.0; // Zap: how far above Pitch the sweep starts
};

enum class NoiseColor : int
{
    White = 0,
    Pink,
    Brown,
    Crackle,
};

struct NoiseParams
{
    NoiseColor color = NoiseColor::White;
    double density = 0.5; // 0..1, Crackle only: 20 Hz .. 20 kHz of impulses
    double width = 0.3;   // 0..1: mono to two independent channels
    FilterMode filter = FilterMode::BandPass;
    double cutoff = 3000.0;
    double reso = 0.2;
    double filterEnvOct = 0.0; // cutoff offset at the hit, decaying to 0
    double envDecayMs = 50.0;  // to -60 dB
    double attackMs = 0.0;
    double holdMs = 0.0;
    double decayMs = 120.0;
    double curve = 0.5;
};

enum class Exciter : int
{
    Impulse = 0,
    Mallet,
    Noise,
};

enum class ResonatorModel : int
{
    Membrane = 0,
    Harmonic,
    Odd,
    Bar,
};

// Damped modes struck by an exciter. Latched when the hit fires, like a
// struck object, except for the level.
struct ResonatorParams
{
    Exciter exciter = Exciter::Mallet;
    ResonatorModel model = ResonatorModel::Membrane;
    int modes = 6;        // 2..8
    double tune = 80.0;   // Hz, the lowest mode
    double keyTrack = 0.0; // 0..1
    double decayMs = 400.0; // T60 of the lowest mode
    double damping = 0.5;   // 0..1: higher modes die faster
    double brightness = 0.4; // 0..1: level of the higher modes
    double hardness = 0.5;   // 0..1: exciter, soft to hard
    double dropSt = 0.0;     // pitch starts this far above Tune ...
    double dropMs = 30.0;    // ... and has fallen onto it after this long
};

// ------------------------------------------------------------- per-hit values

// What a hit fixes when it fires.
struct Hit
{
    double semitones = 0.0;  // note distance from the root
    double level = 1.0;      // velocity and variation, linear
    double pitchRatio = 1.0; // variation
    double timeScale = 1.0;  // variation, scales decays
    uint64_t seed = 0;
};

// The pitch a lane's tonal sources follow: its own Body pitch track, or the
// one of the lane its Pitch Link names. `ratio` carries the transpositions.
struct PitchTrack
{
    double start = 350.0, end = 48.0, sweepMs = 120.0, sweepCurve = 0.55, keyTrack = 0.0;
    double ratio = 1.0;
    const Curve* curve = nullptr;

    double endFreq(double semitones) const;
};

// ---------------------------------------------------------------------- voices

class Voice
{
public:
    bool active() const { return active_; }
    bool fading() const { return fadeStep_ > 0.0; }
    // Fades the voice out over `samples` (a retrigger or a choke).
    void fadeOut(int samples);
    void kill() { active_ = false; }

protected:
    void begin();
    // Steps the fade; false once it has run out and the voice has stopped.
    bool stepFade()
    {
        if (fadeStep_ > 0.0)
        {
            fade_ -= fadeStep_;
            if (fade_ <= 0.0)
            {
                active_ = false;
                return false;
            }
        }
        return true;
    }

    bool active_ = false;
    double fade_ = 1.0;
    double fadeStep_ = 0.0;
};

class BodyVoice : public Voice
{
public:
    void start(const Hit& hit, double phase);
    // Adds n samples into l and r (the body is mono). Every render returns how
    // many of the n samples the voice was still sounding for.
    int render(float* l, float* r, int n, double sampleRate, const BodyParams& p, const PitchTrack& track,
                const Curve& amp);

private:
    static constexpr int kPartials = 8;

    Hit hit_;
    Rng rng_;
    double t_ = 0.0;     // samples since the hit
    double phase_ = 0.0; // turns
    double phase0_ = 0.0;
    double modPhase_ = 0.0;
    std::array<double, kPartials> partialPhase_{};
    double y1_ = 0.0, y2_ = 0.0; // last oscillator outputs, for feedback
    double drift_ = 0.0, driftTarget_ = 0.0;
};

class ClickVoice : public Voice
{
public:
    // Allocates the buffer for the longest click at this rate.
    void prepare(double sampleRate);
    // `pitch` and `cutoff` arrive resolved (transpose, pitch link, variation).
    void start(const Hit& hit, const ClickParams& p, double pitch, double cutoff, double sampleRate);
    int render(float* l, float* r, int n);

private:
    std::vector<float> buffer_;
    int length_ = 0;
    int pos_ = 0;
};

class NoiseVoice : public Voice
{
public:
    void start(const Hit& hit);
    // `freqRatio` scales the filter cutoff (transpose).
    int render(float* l, float* r, int n, double sampleRate, const NoiseParams& p, double freqRatio);

private:
    struct Colour
    {
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0; // pink
        double brown = 0.0;
        double next(NoiseColor c, double white, double crackleChance, Rng& rng);
    };

    Hit hit_;
    Rng rngA_, rngB_;
    Colour colA_, colB_;
    Svf svfL_, svfR_;
    double t_ = 0.0;
};

class ResonatorVoice : public Voice
{
public:
    static constexpr int kMaxModes = 8;

    // `tune` arrives resolved (key track or pitch link, transpose, variation).
    void start(const Hit& hit, const ResonatorParams& p, double tune, double sampleRate);
    int render(float* l, float* r, int n);

private:
    void updateRotations();

    Hit hit_;
    Rng rng_;
    double sampleRate_ = 48000.0;
    int count_ = 0;
    std::array<double, kMaxModes> omega_{}; // radians per sample at Tune
    std::array<double, kMaxModes> radius_{};
    std::array<double, kMaxModes> amp_{};
    std::array<double, kMaxModes> gain_{}; // 0 for a mode above Nyquist
    std::array<double, kMaxModes> rotRe_{}, rotIm_{};
    std::array<double, kMaxModes> zRe_{}, zIm_{};
    double norm_ = 1.0;

    Exciter exciter_ = Exciter::Mallet;
    int exciteLength_ = 1;
    double noiseScale_ = 1.0;
    double noiseLp_ = 0.0, noiseLpCoef_ = 1.0;
    double dropOct_ = 0.0, dropTau_ = 1.0;
    int t_ = 0;
};

} // namespace substrike::dsp
