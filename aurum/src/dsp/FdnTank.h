#pragma once

#include <array>

#include "DecayDesigner.h"
#include "DelayLine.h"
#include "Geq.h"
#include "Math.h"
#include "Modulation.h"

namespace aurum::dsp {

// 16-line feedback delay network, mono in / stereo out.
//
// Feedback matrix: Kronecker product of four 2x2 reflections
//   R(theta) = [cos theta, sin theta; sin theta, -cos theta]
// followed by a fixed permutation. The product is orthogonal for every theta;
// theta = pi/4 gives a (scaled) Hadamard matrix with maximal diffusion, smaller
// angles keep more energy circulating along fixed paths, which is heard as
// discrete late echoes. Each line carries an attenuation GEQ for frequency
// dependent decay and an allpass-interpolated, modulated read tap.
class FdnTank
{
public:
    static constexpr int N = 16;

    void prepare(double sampleRate, double maxDelaySeconds, uint32_t seed);
    void clear();

    // Target line lengths in samples; the tank glides towards them.
    void setTargetDelays(const float* samples);
    void snapDelays();
    void setDiffusionAngle(float theta);
    void setModulation(float depthSamples, float rateHz);

    // Current (smoothed) line lengths, used to request attenuation designs.
    void currentDelays(double* out) const;
    void loadCoeffs(const DecayCoeffSet& set, int offset) { atten_.loadCoeffs(set, offset); }

    void process(const float* in, float* outL, float* outR, int n);

private:
    double fs_ = 48000.0;
    std::array<DelayLine, N> lines_;
    std::array<AllpassInterpolator, N> interp_;
    alignas(32) std::array<float, N> delay_{};
    alignas(32) std::array<float, N> target_{};
    alignas(32) std::array<float, N> inVec_{};
    alignas(32) std::array<float, N> outL_{};
    alignas(32) std::array<float, N> outR_{};
    std::array<int, N> perm_{};
    std::array<SmoothRandom, N> mods_{};
    float modDepth_ = 0.0f;
    float modDepthCurrent_ = 0.0f;
    float glideCoeff_ = 0.0f;
    float c_ = 0.70710678f, s_ = 0.70710678f;
    float maxDelay_ = 0.0f;
    AttenuationBank<N> atten_;
};

} // namespace aurum::dsp
