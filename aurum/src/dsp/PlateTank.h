#pragma once

#include <array>

#include "DecayDesigner.h"
#include "DelayLine.h"
#include "Geq.h"
#include "Math.h"
#include "Modulation.h"
#include "Svf.h"

namespace aurum::dsp {

// Plate reverberator after J. Dattorro, "Effect Design Part 1: Reverberator
// and Other Filters" (JAES, 1997): four input diffusers into a figure-eight
// tank of two halves, each with a modulated decay diffuser, two delays and a
// second decay diffuser. The damping filters of the original are replaced by
// the attenuation GEQ so the Decay Rate EQ applies. Mono in / stereo out.
class PlateTank
{
public:
    static constexpr int kLanes = 8; // two halves used

    void prepare(double sampleRate, double maxScale, uint32_t seed);
    void clear();
    void setScale(float scale);
    void snapScale();
    void setModulation(float depthSamples, float rateHz);
    void setDiffusion(float amount); // input diffusion 0..1

    int numSegments() const { return 2; }
    void currentDelays(double* out) const;
    void loadCoeffs(const DecayCoeffSet& set, int offset) { atten_.loadCoeffs(set, offset); }

    void process(const float* in, float* outL, float* outR, int n);

private:
    float len(float samplesAt29k) const { return samplesAt29k * ratio_ * scale_; }

    double fs_ = 48000.0;
    float ratio_ = 1.0f; // fs / 29761
    float scale_ = 1.0f, scaleTarget_ = 1.0f;
    float glide_ = 0.0f;
    float inDiff1_ = 0.75f, inDiff2_ = 0.625f;
    float modDepth_ = 0.0f, modDepthCur_ = 0.0f;
    std::array<AllpassDiffuser, 4> inputAp_;
    // Tank: [0] = left half, [1] = right half.
    std::array<AllpassDiffuser, 2> modAp_, decayAp_;
    std::array<DelayLine, 2> delayA_, delayB_;
    std::array<SmoothRandom, 2> mods_{};
    std::array<float, 2> modPrev_{};
    std::array<float, 2> halfOut_{};
    AttenuationBank<kLanes> atten_;
    OnePoleCoeffs bandwidth_{};
    OnePoleState bwState_{};
};

} // namespace aurum::dsp
