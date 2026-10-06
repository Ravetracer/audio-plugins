#include "FdnTank.h"

#include <bit>

namespace aurum::dsp {

namespace {

constexpr int kSubBlock = 32;
// Delay changes inside a loop are limited in speed (samples per sample): a
// fast-moving read tap behaves like a parametric pump and can add energy.
constexpr float kMaxGlideRate = 0.03f;
constexpr float kLoopLimit = 16.0f;

float hadamardSign(int row, int col) { return (std::popcount(static_cast<unsigned>(row & col)) & 1) ? -1.0f : 1.0f; }

} // namespace

void FdnTank::prepare(double sampleRate, double maxDelaySeconds, uint32_t seed)
{
    fs_ = sampleRate;
    maxDelay_ = static_cast<float>(maxDelaySeconds * sampleRate);
    for (auto& l : lines_)
        l.allocate(static_cast<int>(maxDelay_) + 16);

    Rng rng(seed);
    const int rows[3] = {3 + static_cast<int>(seed % 3), 6 + static_cast<int>(seed % 2) * 4, 9 + static_cast<int>(seed % 3)};
    const float norm = 1.0f / std::sqrt(static_cast<float>(N));
    for (int i = 0; i < N; ++i)
    {
        inVec_[i] = hadamardSign(rows[0], i) * norm;
        outL_[i] = hadamardSign(rows[1], i) * norm;
        outR_[i] = hadamardSign(rows[2], i) * norm;
        mods_[i].init(seed * 7919u + static_cast<uint32_t>(i) * 104729u + 1u, 0.7f + 0.6f * rng.uniform());
    }
    // Permutation that breaks up the butterfly groups.
    for (int i = 0; i < N; ++i)
        perm_[i] = (i * 5 + 3) % N;

    glideCoeff_ = onePoleCoeff(0.12, sampleRate);
    atten_.setIdentity();
    clear();
}

void FdnTank::clear()
{
    for (auto& l : lines_)
        l.clear();
    for (auto& a : interp_)
        a.reset();
    atten_.reset();
}

void FdnTank::setTargetDelays(const float* samples)
{
    for (int i = 0; i < N; ++i)
        target_[i] = clamp(samples[i], 4.0f, maxDelay_ - 8.0f);
}

void FdnTank::snapDelays() { delay_ = target_; }

void FdnTank::setDiffusionAngle(float theta)
{
    c_ = std::cos(theta);
    s_ = std::sin(theta);
}

void FdnTank::setModulation(float depthSamples, float rateHz)
{
    modDepth_ = depthSamples;
    for (auto& m : mods_)
        m.rate = rateHz;
}

void FdnTank::currentDelays(double* out) const
{
    // Modulation adds 0..2*depth on top of the base length; use the mean.
    for (int i = 0; i < N; ++i)
        out[i] = delay_[i] + modDepthCurrent_;
}

void FdnTank::process(const float* in, float* outL, float* outR, int n)
{
    int done = 0;
    while (done < n)
    {
        const int len = std::min(kSubBlock, n - done);

        // Per-block glide of the base delays and modulation offsets.
        alignas(32) float start[N];
        alignas(32) float step[N];
        const float g = std::pow(glideCoeff_, static_cast<float>(len));
        const float depthStart = modDepthCurrent_;
        modDepthCurrent_ = modDepth_ + (modDepthCurrent_ - modDepth_) * g;
        for (int i = 0; i < N; ++i)
        {
            const float m0 = mods_[i].prev + (mods_[i].next - mods_[i].prev) *
                                                 (mods_[i].phase * mods_[i].phase * (3.0f - 2.0f * mods_[i].phase));
            const float m1 = mods_[i].advance(len, fs_);
            const float d0 = delay_[i] + depthStart * (m0 + 1.0f);
            const float glided = target_[i] + (delay_[i] - target_[i]) * g;
            const float maxStep = kMaxGlideRate * static_cast<float>(len);
            delay_[i] += clamp(glided - delay_[i], -maxStep, maxStep);
            const float d1 = delay_[i] + modDepthCurrent_ * (m1 + 1.0f);
            start[i] = d0;
            step[i] = (d1 - d0) / static_cast<float>(len);
        }

        for (int s = 0; s < len; ++s)
        {
            alignas(32) float x[N];
            for (int i = 0; i < N; ++i)
                x[i] = interp_[i].read(lines_[i], start[i] + step[i] * static_cast<float>(s));

            atten_.process(x);
            // Last-resort guard against runaway (never reached in normal use).
            for (int i = 0; i < N; ++i)
                x[i] = clamp(x[i], -kLoopLimit, kLoopLimit);

            float l = 0.0f, r = 0.0f;
            for (int i = 0; i < N; ++i)
            {
                l += outL_[i] * x[i];
                r += outR_[i] * x[i];
            }

            for (int h = 1; h < N; h <<= 1)
                for (int j = 0; j < N; j += 2 * h)
                    for (int i = j; i < j + h; ++i)
                    {
                        const float a = x[i];
                        const float b = x[i + h];
                        x[i] = c_ * a + s_ * b;
                        x[i + h] = s_ * a - c_ * b;
                    }

            const float v = in[done + s];
            for (int i = 0; i < N; ++i)
                lines_[i].push(x[perm_[i]] + inVec_[i] * v);

            outL[done + s] = l;
            outR[done + s] = r;
        }
        done += len;
    }
}

} // namespace aurum::dsp
