#include "EarlyReflections.h"

#include <algorithm>

namespace aurum::dsp {

void EarlyReflections::prepare(double sampleRate, double maxSeconds)
{
    fs_ = sampleRate;
    maxDelay_ = static_cast<float>(maxSeconds * sampleRate);
    lineL_.allocate(static_cast<int>(maxDelay_) + 8);
    lineR_.allocate(static_cast<int>(maxDelay_) + 8);
    for (auto& d : diff_)
        d.allocate(static_cast<int>(0.02 * sampleRate) + 8);
    diffLen_ = {static_cast<float>(0.0031 * sampleRate), static_cast<float>(0.0047 * sampleRate)};

    auto makePattern = [](std::array<Tap, kTaps>& taps, uint32_t seed) {
        Rng rng(seed);
        for (int j = 0; j < kTaps; ++j)
        {
            // Reflections get denser over time: u grows sub-linearly with j.
            const float base = std::pow((j + 0.5f) / kTaps, 1.35f);
            const float jitter = (rng.uniform() - 0.5f) * 0.8f / kTaps;
            taps[static_cast<size_t>(j)].u = clamp(base + jitter, 0.0f, 1.0f);
            const float sign = (rng.next() & 1) ? 1.0f : -1.0f;
            const float level = 0.55f + 0.45f * rng.uniform();
            taps[static_cast<size_t>(j)].gain = sign * level * std::exp(-2.2f * taps[static_cast<size_t>(j)].u);
            taps[static_cast<size_t>(j)].cross = (rng.uniform() < 0.3f) && j > 0;
        }
        std::sort(taps.begin(), taps.end(), [](const Tap& a, const Tap& b) { return a.u < b.u; });
    };
    makePattern(tapsL_, 0x51ed270bu);
    makePattern(tapsR_, 0x2545f491u);

    glide_ = onePoleCoeff(0.12, sampleRate);
    tone_ = SvfCoeffs::lowPass(8000.0, 0.7071, fs_);
    updateGains();
    clear();
}

void EarlyReflections::clear()
{
    lineL_.clear();
    lineR_.clear();
    for (auto& d : diff_)
        d.clear();
    toneL_.reset();
    toneR_.reset();
}

void EarlyReflections::setShape(double lengthMs, double startMs, double sparsity, double diffusion, double toneHz)
{
    lengthTarget_ = static_cast<float>(lengthMs * 1e-3 * fs_);
    startTarget_ = static_cast<float>(startMs * 1e-3 * fs_);
    const float sp = static_cast<float>(clamp(sparsity, 0.0, 1.0));
    if (sp != sparsity_)
    {
        sparsity_ = sp;
        updateGains();
    }
    diffusion_ = static_cast<float>(clamp(diffusion, 0.0, 0.85));
    tone_ = SvfCoeffs::lowPass(toneHz, 0.6, fs_);
}

void EarlyReflections::snap()
{
    length_ = lengthTarget_;
    start_ = startTarget_;
}

void EarlyReflections::updateGains()
{
    // Sparsity fades out the weaker taps; the sum of squares stays at one.
    auto build = [&](const std::array<Tap, kTaps>& taps, std::array<float, kTaps>& out) {
        float energy = 0.0f;
        for (int j = 0; j < kTaps; ++j)
        {
            const float rank = static_cast<float>(j) / kTaps;
            const float keep = clamp(1.0f - (rank - (1.0f - sparsity_)) * 4.0f, 0.0f, 1.0f);
            const float emphasis = 1.0f + sparsity_ * (1.0f - rank);
            out[static_cast<size_t>(j)] = taps[static_cast<size_t>(j)].gain * keep * emphasis;
            energy += out[static_cast<size_t>(j)] * out[static_cast<size_t>(j)];
        }
        const float norm = energy > 0.0f ? 1.0f / std::sqrt(energy) : 0.0f;
        for (auto& g : out)
            g *= norm;
    };
    build(tapsL_, gainL_);
    build(tapsR_, gainR_);
}

void EarlyReflections::process(const float* inL, const float* inR, float* outL, float* outR, int n)
{
    // Glide once per call; callers use small blocks.
    const float g = std::pow(glide_, static_cast<float>(n));
    const float len0 = length_, st0 = start_;
    length_ = lengthTarget_ + (length_ - lengthTarget_) * g;
    start_ = startTarget_ + (start_ - startTarget_) * g;
    const float dLen = (length_ - len0) / static_cast<float>(n);
    const float dSt = (start_ - st0) / static_cast<float>(n);

    for (int s = 0; s < n; ++s)
    {
        lineL_.push(inL[s]);
        lineR_.push(inR[s]);
        const float len = len0 + dLen * static_cast<float>(s + 1);
        const float st = st0 + dSt * static_cast<float>(s + 1);
        float l = 0.0f, r = 0.0f;
        for (int j = 0; j < kTaps; ++j)
        {
            const Tap& tl = tapsL_[static_cast<size_t>(j)];
            const float dl = clamp(st + tl.u * len, 1.0f, maxDelay_ - 4.0f);
            l += gainL_[static_cast<size_t>(j)] * (tl.cross ? lineR_ : lineL_).readLinear(dl);
            const Tap& tr = tapsR_[static_cast<size_t>(j)];
            const float dr = clamp(st + tr.u * len, 1.0f, maxDelay_ - 4.0f);
            r += gainR_[static_cast<size_t>(j)] * (tr.cross ? lineL_ : lineR_).readLinear(dr);
        }
        // Light diffusion: two short allpasses per side.
        l = diff_[0].processFrac(l, diffLen_[0], diffusion_);
        l = diff_[1].processFrac(l, diffLen_[1], diffusion_ * 0.8f);
        r = diff_[2].processFrac(r, diffLen_[0] * 1.13f, diffusion_);
        r = diff_[3].processFrac(r, diffLen_[1] * 0.91f, diffusion_ * 0.8f);
        outL[s] = toneL_.process(l, tone_);
        outR[s] = toneR_.process(r, tone_);
    }
}

} // namespace aurum::dsp
