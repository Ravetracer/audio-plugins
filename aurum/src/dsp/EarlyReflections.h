#pragma once

#include <array>

#include "DelayLine.h"
#include "Math.h"
#include "Svf.h"

namespace aurum::dsp {

// Stereo early reflection generator: a sparse multitap pattern whose span and
// onset follow the room, followed by a light diffusion and tone stage.
//
// The tap pattern is fixed (seeded) and expressed in normalised time, so that
// sweeping the Space control stretches it smoothly instead of switching
// patterns. A share of the taps reads from the opposite input channel for
// lateral spread.
class EarlyReflections
{
public:
    static constexpr int kTaps = 28;

    void prepare(double sampleRate, double maxSeconds);
    void clear();

    // lengthMs: span of the pattern, startMs: first reflection, sparsity 0..1
    // (0 = dense, 1 = only the strongest few taps), diffusion 0..1,
    // toneHz: lowpass corner of the reflections.
    void setShape(double lengthMs, double startMs, double sparsity, double diffusion, double toneHz);
    void snap();

    void process(const float* inL, const float* inR, float* outL, float* outR, int n);

private:
    struct Tap
    {
        float u;     // normalised time 0..1
        float gain;  // signed gain
        bool cross;  // reads the opposite channel
    };

    void updateGains();

    double fs_ = 48000.0;
    DelayLine lineL_, lineR_;
    std::array<Tap, kTaps> tapsL_{}, tapsR_{};
    std::array<float, kTaps> gainL_{}, gainR_{};
    float length_ = 0.0f, lengthTarget_ = 0.0f;
    float start_ = 0.0f, startTarget_ = 0.0f;
    float sparsity_ = 0.0f;
    float diffusion_ = 0.5f;
    float glide_ = 0.0f;
    float maxDelay_ = 0.0f;
    std::array<AllpassDiffuser, 4> diff_;
    std::array<float, 2> diffLen_{};
    SvfCoeffs tone_;
    SvfState toneL_, toneR_;
};

} // namespace aurum::dsp
