#include "PostEq.h"

namespace aurum::dsp {

namespace {

int slopeOrder(int slopeIndex) { return kSlopeDb[clamp(slopeIndex, 0, kNumSlopes - 1)] / 6; }

// Butterworth section Qs for a given order: the second-order sections, plus
// whether a first-order section is needed.
int butterworthQs(int order, double* qs, bool& onePole)
{
    onePole = (order & 1) != 0;
    const int pairs = order / 2;
    for (int i = 0; i < pairs; ++i)
    {
        const double theta = onePole ? kPi * (i + 1) / order : kPi * (2 * i + 1) / (2.0 * order);
        qs[i] = 1.0 / (2.0 * std::cos(theta));
    }
    return pairs;
}

} // namespace

void PostEq::prepare(double sampleRate)
{
    fs_ = sampleRate;
    smooth_ = onePoleCoeff(0.02, sampleRate);
    for (auto& b : bands_)
    {
        b.current = b.target;
        b.dirty = true;
    }
    reset();
}

void PostEq::reset()
{
    for (auto& b : bands_)
    {
        for (auto& ch : b.svf)
            for (auto& s : ch)
                s.reset();
        for (auto& s : b.ops)
            s.reset();
    }
}

void PostEq::setBands(const std::array<PostBand, kNumPostBands>& bands)
{
    for (int i = 0; i < kNumPostBands; ++i)
    {
        BandState& b = bands_[static_cast<size_t>(i)];
        const PostBand& t = bands[static_cast<size_t>(i)];
        const bool structural = t.shape != b.target.shape || t.slope != b.target.slope ||
                                t.placement != b.target.placement || t.used != b.target.used ||
                                t.enabled != b.target.enabled;
        b.target = t;
        if (structural)
        {
            // Shape changes cannot be smoothed; restart this band's states.
            b.current = t;
            for (auto& ch : b.svf)
                for (auto& s : ch)
                    s.reset();
            for (auto& s : b.ops)
                s.reset();
            b.dirty = true;
        }
    }
    compTarget_ = static_cast<float>(dbToGain(compensationDb(bands, fs_)));
}

void PostEq::design(BandState& b)
{
    const PostBand& p = b.current;
    b.active = p.used && p.enabled;
    b.numStages = 0;
    b.onePole = false;
    if (!b.active)
        return;
    const double f = clamp(p.freq, 10.0, fs_ * 0.49);
    switch (p.shape)
    {
    case PostShape::Bell:
        b.coeffs[0] = SvfCoeffs::bell(f, p.q, p.gainDb, fs_);
        b.numStages = 1;
        break;
    case PostShape::LowShelf:
        b.coeffs[0] = SvfCoeffs::lowShelf(f, p.q * kShelfQScale, p.gainDb, fs_);
        b.numStages = 1;
        break;
    case PostShape::HighShelf:
        b.coeffs[0] = SvfCoeffs::highShelf(f, p.q * kShelfQScale, p.gainDb, fs_);
        b.numStages = 1;
        break;
    case PostShape::LowCut:
    case PostShape::HighCut:
    {
        double qs[kMaxStages];
        bool onePole = false;
        const int pairs = butterworthQs(slopeOrder(p.slope), qs, onePole);
        // The band Q sets the resonance of the sharpest section; Q 1 is a
        // plain Butterworth response.
        if (pairs > 0)
            qs[pairs - 1] *= p.q;
        const bool high = p.shape == PostShape::LowCut;
        for (int i = 0; i < pairs; ++i)
            b.coeffs[static_cast<size_t>(i)] = high ? SvfCoeffs::highPass(f, qs[i], fs_) : SvfCoeffs::lowPass(f, qs[i], fs_);
        b.numStages = pairs;
        b.onePole = onePole;
        b.onePoleHigh = high;
        b.op = OnePoleCoeffs::make(f, fs_);
        break;
    }
    }
}

void PostEq::processChannel(BandState& b, int ch, float* x, int n)
{
    auto& states = b.svf[static_cast<size_t>(ch)];
    for (int st = 0; st < b.numStages; ++st)
    {
        SvfState& s = states[static_cast<size_t>(st)];
        const SvfCoeffs& c = b.coeffs[static_cast<size_t>(st)];
        for (int i = 0; i < n; ++i)
            x[i] = s.process(x[i], c);
    }
    if (b.onePole)
    {
        OnePoleState& s = b.ops[static_cast<size_t>(ch)];
        if (b.onePoleHigh)
            for (int i = 0; i < n; ++i)
                x[i] = s.highpass(x[i], b.op);
        else
            for (int i = 0; i < n; ++i)
                x[i] = s.lowpass(x[i], b.op);
    }
}

void PostEq::process(float* L, float* R, int n)
{
    // Control-rate smoothing of continuous band parameters.
    const float g = std::pow(smooth_, static_cast<float>(n));
    for (auto& b : bands_)
    {
        PostBand& c = b.current;
        const PostBand& t = b.target;
        if (std::fabs(c.freq - t.freq) > 1e-6 * t.freq || std::fabs(c.gainDb - t.gainDb) > 1e-5 ||
            std::fabs(c.q - t.q) > 1e-6)
        {
            c.freq = t.freq * std::pow(c.freq / t.freq, static_cast<double>(g));
            c.gainDb = t.gainDb + (c.gainDb - t.gainDb) * g;
            c.q = t.q * std::pow(c.q / t.q, static_cast<double>(g));
            b.dirty = true;
        }
        if (b.dirty)
        {
            design(b);
            b.dirty = false;
        }
    }

    alignas(32) float tmp[2][64];
    for (int off = 0; off < n; off += 64)
    {
        const int len = std::min(64, n - off);
        float* l = L + off;
        float* r = R + off;
        for (auto& b : bands_)
        {
            if (!b.active)
                continue;
            switch (b.current.placement)
            {
            case Placement::Stereo:
                processChannel(b, 0, l, len);
                processChannel(b, 1, r, len);
                break;
            case Placement::Left: processChannel(b, 0, l, len); break;
            case Placement::Right: processChannel(b, 1, r, len); break;
            case Placement::Mid:
            case Placement::Side:
            {
                for (int i = 0; i < len; ++i)
                {
                    tmp[0][i] = 0.5f * (l[i] + r[i]);
                    tmp[1][i] = 0.5f * (l[i] - r[i]);
                }
                const int which = b.current.placement == Placement::Mid ? 0 : 1;
                processChannel(b, 0, tmp[which], len);
                for (int i = 0; i < len; ++i)
                {
                    l[i] = tmp[0][i] + tmp[1][i];
                    r[i] = tmp[0][i] - tmp[1][i];
                }
                break;
            }
            }
        }
        for (int i = 0; i < len; ++i)
        {
            compGain_ = compTarget_ + (compGain_ - compTarget_) * smooth_;
            l[i] *= compGain_;
            r[i] *= compGain_;
        }
    }
}

double PostEq::bandPow(const PostBand& b, double f, double fs)
{
    if (!b.used || !b.enabled)
        return 1.0;
    const double f0 = clamp(b.freq, 10.0, fs * 0.49);
    switch (b.shape)
    {
    case PostShape::Bell: return response::bellPow(f, f0, b.q, b.gainDb, fs);
    case PostShape::LowShelf: return response::lowShelfPow(f, f0, b.q * kShelfQScale, b.gainDb, fs);
    case PostShape::HighShelf: return response::highShelfPow(f, f0, b.q * kShelfQScale, b.gainDb, fs);
    case PostShape::LowCut:
    case PostShape::HighCut:
    {
        double qs[kMaxStages];
        bool onePole = false;
        const int pairs = butterworthQs(slopeOrder(b.slope), qs, onePole);
        if (pairs > 0)
            qs[pairs - 1] *= b.q;
        const bool high = b.shape == PostShape::LowCut;
        double p = 1.0;
        for (int i = 0; i < pairs; ++i)
            p *= high ? response::highPassPow(f, f0, qs[i], fs) : response::lowPassPow(f, f0, qs[i], fs);
        if (onePole)
            p *= high ? response::onePoleHighPow(f, f0, fs) : response::onePoleLowPow(f, f0, fs);
        return p;
    }
    }
    return 1.0;
}

double PostEq::channelPow(const std::array<PostBand, kNumPostBands>& bands, double f, double fs, int channel)
{
    double p = 1.0;
    for (const auto& b : bands)
    {
        if (!b.used || !b.enabled)
            continue;
        const double h = bandPow(b, f, fs);
        switch (b.placement)
        {
        case Placement::Stereo: p *= h; break;
        case Placement::Left: p *= channel == 0 ? h : 1.0; break;
        case Placement::Right: p *= channel == 1 ? h : 1.0; break;
        case Placement::Mid:
        case Placement::Side: p *= 0.5 * (1.0 + h); break;
        }
    }
    return p;
}

double PostEq::compensationDb(const std::array<PostBand, kNumPostBands>& bands, double fs)
{
    bool any = false;
    for (const auto& b : bands)
        any |= b.used && b.enabled;
    if (!any)
        return 0.0;
    // Equal weight per octave (pink) between 40 Hz and 12 kHz.
    constexpr int kPoints = 48;
    double sum = 0.0;
    for (int i = 0; i < kPoints; ++i)
    {
        const double f = 40.0 * std::pow(300.0, (i + 0.5) / kPoints);
        sum += 0.5 * (channelPow(bands, f, fs, 0) + channelPow(bands, f, fs, 1));
    }
    const double mean = sum / kPoints;
    return clamp(-10.0 * std::log10(std::max(mean, 1e-6)), -12.0, 12.0);
}

} // namespace aurum::dsp
