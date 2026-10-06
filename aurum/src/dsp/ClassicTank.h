#pragma once

#include <array>

#include "DecayDesigner.h"
#include "DelayLine.h"
#include "Geq.h"
#include "Math.h"
#include "Modulation.h"
#include "Svf.h"

namespace aurum::dsp {

// Ring of four allpass/delay branches in the style of early digital hall
// algorithms: slower echo density build-up than the FDN, band-limited, and a
// randomly "wandering" modulated allpass per branch that gives the tail its
// shimmer. Mono in / stereo out.
class ClassicTank
{
public:
    static constexpr int kBranches = 4;
    static constexpr int kLanes = 8; // attenuation bank width (4 used)

    void prepare(double sampleRate, double maxScale, uint32_t seed);
    void clear();
    void setScale(float scale); // relative size, glides
    void snapScale();
    void setModulation(float depthSamples, float rateHz);
    void setDiffusion(float amount); // 0..1

    int numSegments() const { return kBranches; }
    void currentDelays(double* out) const;
    void loadCoeffs(const DecayCoeffSet& set, int offset) { atten_.loadCoeffs(set, offset); }

    void process(const float* in, float* outL, float* outR, int n);

private:
    double fs_ = 48000.0;
    float scale_ = 1.0f, scaleTarget_ = 1.0f;
    float glide_ = 0.0f;
    float modDepth_ = 0.0f, modDepthCur_ = 0.0f;
    float apG_ = 0.6f;
    std::array<AllpassDiffuser, kBranches> ap1_, ap2_;
    std::array<DelayLine, kBranches> delay_;
    std::array<float, kBranches> ap1Len_{}, ap2Len_{}, delayLen_{};
    std::array<SmoothRandom, kBranches> mods_{};
    std::array<float, kBranches> modPrev_{};
    std::array<float, kBranches> branchOut_{};
    AttenuationBank<kLanes> atten_;
    OnePoleCoeffs bandwidth_{};
    OnePoleState bwState_{};
    SvfCoeffs outLp_{};
    std::array<SvfState, 2> outLpS_{};
};

} // namespace aurum::dsp
