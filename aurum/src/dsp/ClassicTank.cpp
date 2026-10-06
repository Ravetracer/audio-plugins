#include "ClassicTank.h"

namespace aurum::dsp {

namespace {

// Base lengths in milliseconds at scale 1 (a medium hall).
constexpr float kAp1Ms[4] = {4.71f, 5.93f, 6.61f, 7.87f};
constexpr float kAp2Ms[4] = {22.6f, 30.1f, 27.3f, 35.4f};
constexpr float kDelayMs[4] = {89.5f, 105.7f, 96.3f, 121.2f};
// Output taps: (branch, position as fraction of the delay, sign).
struct Tap
{
    int branch;
    float pos;
    float sign;
};
constexpr Tap kTapsL[6] = {{0, 0.11f, 1.f}, {0, 0.63f, 1.f}, {1, 0.37f, -1.f}, {2, 0.21f, 1.f}, {2, 0.84f, -1.f}, {3, 0.52f, -1.f}};
constexpr Tap kTapsR[6] = {{2, 0.15f, 1.f}, {2, 0.58f, 1.f}, {3, 0.29f, -1.f}, {0, 0.44f, 1.f}, {0, 0.91f, -1.f}, {1, 0.71f, -1.f}};
constexpr int kSubBlock = 32;
constexpr float kMaxRelGlide = 1.0e-5f; // relative size change per sample
constexpr float kLoopLimit = 16.0f;

} // namespace

void ClassicTank::prepare(double sampleRate, double maxScale, uint32_t seed)
{
    fs_ = sampleRate;
    const double ms = sampleRate * 1e-3;
    for (int b = 0; b < kBranches; ++b)
    {
        ap1_[b].allocate(static_cast<int>(kAp1Ms[b] * maxScale * ms) + 64);
        ap2_[b].allocate(static_cast<int>(kAp2Ms[b] * maxScale * ms + 0.01 * sampleRate) + 64);
        delay_[b].allocate(static_cast<int>(kDelayMs[b] * maxScale * ms) + 64);
        mods_[b].init(seed * 31u + static_cast<uint32_t>(b) * 977u + 7u, 0.75f + 0.15f * b);
    }
    glide_ = onePoleCoeff(0.12, sampleRate);
    bandwidth_ = OnePoleCoeffs::make(9500.0, sampleRate);
    outLp_ = SvfCoeffs::lowPass(11000.0, 0.6, sampleRate);
    atten_.setIdentity();
    clear();
}

void ClassicTank::clear()
{
    for (int b = 0; b < kBranches; ++b)
    {
        ap1_[b].clear();
        ap2_[b].clear();
        delay_[b].clear();
    }
    branchOut_.fill(0.0f);
    atten_.reset();
    bwState_.reset();
    for (auto& s : outLpS_)
        s.reset();
}

void ClassicTank::setScale(float scale) { scaleTarget_ = clamp(scale, 0.05f, 2.0f); }

void ClassicTank::snapScale() { scale_ = scaleTarget_; }

void ClassicTank::setModulation(float depthSamples, float rateHz)
{
    modDepth_ = depthSamples;
    for (auto& m : mods_)
        m.rate = rateHz;
}

void ClassicTank::setDiffusion(float amount) { apG_ = 0.45f + 0.25f * clamp(amount, 0.0f, 1.0f); }

void ClassicTank::currentDelays(double* out) const
{
    const double ms = fs_ * 1e-3;
    for (int b = 0; b < kBranches; ++b)
        out[b] = (kAp1Ms[b] + kAp2Ms[b] + kDelayMs[b]) * scale_ * ms + modDepthCur_;
}

void ClassicTank::process(const float* in, float* outL, float* outR, int n)
{
    const float ms = static_cast<float>(fs_ * 1e-3);
    int done = 0;
    while (done < n)
    {
        const int len = std::min(kSubBlock, n - done);
        const float g = std::pow(glide_, static_cast<float>(len));
        {
            // Rate-limited glide of the structure size (relative change per
            // sample), see FdnTank.
            const float glided = scaleTarget_ + (scale_ - scaleTarget_) * g;
            const float maxStep = kMaxRelGlide * static_cast<float>(len);
            scale_ += clamp(glided - scale_, -maxStep * scale_, maxStep * scale_);
        }
        const float depth0 = modDepthCur_;
        modDepthCur_ = modDepth_ + (modDepthCur_ - modDepth_) * g;

        float ap2Start[kBranches], ap2Step[kBranches];
        for (int b = 0; b < kBranches; ++b)
        {
            ap1Len_[b] = kAp1Ms[b] * scale_ * ms;
            delayLen_[b] = kDelayMs[b] * scale_ * ms;
            const float base = kAp2Ms[b] * scale_ * ms;
            const float m0 = modPrev_[b];
            const float m1 = mods_[b].advance(len, fs_);
            modPrev_[b] = m1;
            const float d0 = base + depth0 * (m0 + 1.0f);
            const float d1 = base + modDepthCur_ * (m1 + 1.0f);
            ap2Start[b] = d0;
            ap2Step[b] = (d1 - d0) / static_cast<float>(len);
        }

        for (int s = 0; s < len; ++s)
        {
            const float x = bwState_.lowpass(in[done + s], bandwidth_);
            // Each branch is fed by the attenuated output of the previous
            // branch from the last sample (one extra sample per branch keeps
            // the attenuation of all branches in a single vector pass).
            alignas(32) float seg[kLanes] = {};
            for (int b = 0; b < kBranches; ++b)
            {
                float v = branchOut_[static_cast<size_t>((b + kBranches - 1) % kBranches)];
                if (b == 0 || b == 2)
                    v += x;
                const float sign = (b & 1) ? -1.0f : 1.0f;
                v = ap1_[b].processNormalised(v, ap1Len_[b], sign * apG_);
                v = ap2_[b].processNormalised(v, ap2Start[b] + ap2Step[b] * static_cast<float>(s), -sign * apG_);
                seg[b] = delay_[b].readHermite(delayLen_[b]);
                delay_[b].push(v);
            }
            atten_.process(seg);
            for (int b = 0; b < kBranches; ++b)
                branchOut_[static_cast<size_t>(b)] = clamp(seg[b], -kLoopLimit, kLoopLimit);

            float l = 0.0f, r = 0.0f;
            for (const Tap& t : kTapsL)
                l += t.sign * delay_[t.branch].readLinear(delayLen_[t.branch] * t.pos + 1.0f);
            for (const Tap& t : kTapsR)
                r += t.sign * delay_[t.branch].readLinear(delayLen_[t.branch] * t.pos + 1.0f);
            outL[done + s] = outLpS_[0].process(l * 0.4f, outLp_);
            outR[done + s] = outLpS_[1].process(r * 0.4f, outLp_);
        }
        done += len;
    }
}

} // namespace aurum::dsp
