#include "PlateTank.h"

namespace aurum::dsp {

namespace {

constexpr float kRefRate = 29761.0f;
// Lengths at 29.761 kHz (Dattorro 1997).
constexpr float kInputAp[4] = {142.0f, 107.0f, 379.0f, 277.0f};
constexpr float kModAp[2] = {672.0f, 908.0f};
constexpr float kDelayA[2] = {4453.0f, 4217.0f};
constexpr float kDecayAp[2] = {1800.0f, 2656.0f};
constexpr float kDelayB[2] = {3720.0f, 3163.0f};
constexpr float kDecayDiffusion1 = 0.70f;
constexpr float kDecayDiffusion2 = 0.50f;
constexpr int kSubBlock = 32;
constexpr float kMaxRelGlide = 1.0e-5f; // relative size change per sample
constexpr float kLoopLimit = 16.0f;

// Output taps (Dattorro, table 2): half, element (0 = delayA, 1 = decayAp,
// 2 = delayB), position at 29.761 kHz, sign.
struct Tap
{
    int half;
    int element;
    float pos;
    float sign;
};
constexpr Tap kTapsL[7] = {{1, 0, 266.f, 1.f},  {1, 0, 2974.f, 1.f}, {1, 1, 1913.f, -1.f}, {1, 2, 1996.f, 1.f},
                           {0, 0, 1990.f, -1.f}, {0, 1, 187.f, -1.f}, {0, 2, 1066.f, -1.f}};
constexpr Tap kTapsR[7] = {{0, 0, 353.f, 1.f},  {0, 0, 3627.f, 1.f}, {0, 1, 1228.f, -1.f}, {0, 2, 2673.f, 1.f},
                           {1, 0, 2111.f, -1.f}, {1, 1, 335.f, -1.f}, {1, 2, 121.f, -1.f}};

} // namespace

void PlateTank::prepare(double sampleRate, double maxScale, uint32_t seed)
{
    fs_ = sampleRate;
    ratio_ = static_cast<float>(sampleRate / kRefRate);
    const float m = static_cast<float>(maxScale) * ratio_;
    for (int i = 0; i < 4; ++i)
        inputAp_[static_cast<size_t>(i)].allocate(static_cast<int>(kInputAp[i] * ratio_) + 16);
    for (int h = 0; h < 2; ++h)
    {
        modAp_[static_cast<size_t>(h)].allocate(static_cast<int>(kModAp[h] * m + 0.01 * sampleRate) + 32);
        decayAp_[static_cast<size_t>(h)].allocate(static_cast<int>(kDecayAp[h] * m) + 32);
        delayA_[static_cast<size_t>(h)].allocate(static_cast<int>(kDelayA[h] * m) + 32);
        delayB_[static_cast<size_t>(h)].allocate(static_cast<int>(kDelayB[h] * m) + 32);
        mods_[static_cast<size_t>(h)].init(seed * 131u + static_cast<uint32_t>(h) * 7u + 3u, 0.9f + 0.2f * h);
    }
    glide_ = onePoleCoeff(0.12, sampleRate);
    bandwidth_ = OnePoleCoeffs::make(14000.0, sampleRate);
    atten_.setIdentity();
    clear();
}

void PlateTank::clear()
{
    for (auto& a : inputAp_)
        a.clear();
    for (int h = 0; h < 2; ++h)
    {
        modAp_[static_cast<size_t>(h)].clear();
        decayAp_[static_cast<size_t>(h)].clear();
        delayA_[static_cast<size_t>(h)].clear();
        delayB_[static_cast<size_t>(h)].clear();
    }
    halfOut_.fill(0.0f);
    atten_.reset();
    bwState_.reset();
}

void PlateTank::setScale(float scale) { scaleTarget_ = clamp(scale, 0.3f, 2.0f); }

void PlateTank::snapScale() { scale_ = scaleTarget_; }

void PlateTank::setModulation(float depthSamples, float rateHz)
{
    modDepth_ = depthSamples;
    for (auto& m : mods_)
        m.rate = rateHz;
}

void PlateTank::setDiffusion(float amount)
{
    const float a = clamp(amount, 0.0f, 1.0f);
    inDiff1_ = 0.55f + 0.25f * a;
    inDiff2_ = 0.45f + 0.22f * a;
}

void PlateTank::currentDelays(double* out) const
{
    for (int h = 0; h < 2; ++h)
        out[h] = len(kModAp[h] + kDelayA[h] + kDecayAp[h] + kDelayB[h]) + modDepthCur_;
}

void PlateTank::process(const float* in, float* outL, float* outR, int n)
{
    int done = 0;
    while (done < n)
    {
        const int blk = std::min(kSubBlock, n - done);
        const float g = std::pow(glide_, static_cast<float>(blk));
        {
            // Rate-limited glide of the structure size (relative change per
            // sample), see FdnTank.
            const float glided = scaleTarget_ + (scale_ - scaleTarget_) * g;
            const float maxStep = kMaxRelGlide * static_cast<float>(blk);
            scale_ += clamp(glided - scale_, -maxStep * scale_, maxStep * scale_);
        }
        const float depth0 = modDepthCur_;
        modDepthCur_ = modDepth_ + (modDepthCur_ - modDepth_) * g;

        float modStart[2], modStep[2], dA[2], dB[2], dAp[2];
        for (int h = 0; h < 2; ++h)
        {
            const float base = len(kModAp[h]);
            const float m0 = modPrev_[static_cast<size_t>(h)];
            const float m1 = mods_[static_cast<size_t>(h)].advance(blk, fs_);
            modPrev_[static_cast<size_t>(h)] = m1;
            modStart[h] = base + depth0 * (m0 + 1.0f);
            modStep[h] = (base + modDepthCur_ * (m1 + 1.0f) - modStart[h]) / static_cast<float>(blk);
            dA[h] = len(kDelayA[h]);
            dB[h] = len(kDelayB[h]);
            dAp[h] = len(kDecayAp[h]);
        }
        const int inLen[4] = {static_cast<int>(kInputAp[0] * ratio_), static_cast<int>(kInputAp[1] * ratio_),
                              static_cast<int>(kInputAp[2] * ratio_), static_cast<int>(kInputAp[3] * ratio_)};

        for (int s = 0; s < blk; ++s)
        {
            float x = bwState_.lowpass(in[done + s], bandwidth_);
            x = inputAp_[0].process(x, inLen[0], inDiff1_);
            x = inputAp_[1].process(x, inLen[1], inDiff1_);
            x = inputAp_[2].process(x, inLen[2], inDiff2_);
            x = inputAp_[3].process(x, inLen[3], inDiff2_);

            alignas(32) float seg[kLanes] = {};
            for (int h = 0; h < 2; ++h)
            {
                // Each half is fed by the other half's output (figure eight).
                float v = x + halfOut_[static_cast<size_t>(1 - h)];
                v = modAp_[static_cast<size_t>(h)].processNormalised(
                    v, modStart[h] + modStep[h] * static_cast<float>(s), -kDecayDiffusion1);
                const float a = delayA_[static_cast<size_t>(h)].readHermite(dA[h]);
                delayA_[static_cast<size_t>(h)].push(v);
                seg[h] = a;
            }
            atten_.process(seg);
            for (int h = 0; h < 2; ++h)
            {
                float v = decayAp_[static_cast<size_t>(h)].processNormalised(seg[h], dAp[h], kDecayDiffusion2);
                const float b = delayB_[static_cast<size_t>(h)].readHermite(dB[h]);
                delayB_[static_cast<size_t>(h)].push(v);
                halfOut_[static_cast<size_t>(h)] = clamp(b, -kLoopLimit, kLoopLimit);
            }

            auto readTap = [&](const Tap& t) {
                const float d = len(t.pos) + 1.0f;
                switch (t.element)
                {
                case 0: return delayA_[static_cast<size_t>(t.half)].readLinear(d);
                case 1: return decayAp_[static_cast<size_t>(t.half)].line().readLinear(d);
                default: return delayB_[static_cast<size_t>(t.half)].readLinear(d);
                }
            };
            float l = 0.0f, r = 0.0f;
            for (const Tap& t : kTapsL)
                l += t.sign * readTap(t);
            for (const Tap& t : kTapsR)
                r += t.sign * readTap(t);
            outL[done + s] = l * 0.6f;
            outR[done + s] = r * 0.6f;
        }
        done += blk;
    }
}

} // namespace aurum::dsp
